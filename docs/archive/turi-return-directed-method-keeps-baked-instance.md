# `--interpret` keeps the baked instance for a RETURN-directed class method

**Severity:** medium. Silent wrong answers under `--interpret` / `tur repl` for
any constrained generic that calls a return-directed class method (`pure`,
`default-of`, ...). The compiled path is correct, so this is an
interpreter/compiled divergence, not a miscompile.

**Status:** open. Re-verified 2026-08-01 -- every repro below still reproduces
on `main`. Found 2026-07-30 while checking whether
`hkt-constrained-byvalue-bind-pure` (red under `run-turi`) was caused by local
changes. It is not -- see [Nothing regressed](#nothing-regressed-a-carve-out-lifted).

## Scope -- this report absorbs two symptom reports (2026-08-01)

Two earlier reports described the *same defect* from the symptom end, both
against the single fixture that surfaces it. They are archived; their content
is folded in here, and this file is the one to read.

| Archived | What it was |
| --- | --- |
| [`turi-hkt-constrained-byvalue-bind-pure-wrong-values`](../archive/turi-hkt-constrained-byvalue-bind-pure-wrong-values.md) | filed 2026-07-30: `hkt-constrained-byvalue-bind-pure` prints `1 / -1 / 1` under turi |
| [`turi-hkt-byvalue-bind-pure-wrong-value`](../archive/turi-hkt-byvalue-bind-pure-wrong-value.md) | filed 2026-07-31: the same fixture, the same three numbers, re-discovered a day later |

Both read the wrong `1` as a mis-slotted by-value `Option` carrier -- an
interpreter *readback* bug -- and pointed at
`src/turi/interpreter_natives.c`'s option/result carrier helpers. That
direction is wrong, and the payload-independence probe below is what rules it
out: `(pure-n (some 0) 7)` and `(pure-n (some 0) 99)` both interpret to `1`,
so no `pure` is running at all and there is no payload to mis-slot. The `1` is
the baked representative instance answering, not a misread value word.

One claim from the first of the two should not be carried forward: it says
"the sibling `hkt-constrained-continuation-dict` passes under turi, so plain
instance selection is fine." That fixture carries an inline-C block and is
PASS-**skipped** by the TI7 carve-out -- it is never executed. Run directly it
gives `207 207` where `107 207` is wanted, which is this same defect. See
[Nothing regressed](#nothing-regressed-a-carve-out-lifted).

Both archived reports also independently established that the failure is
**pre-existing**, each against a different suspect change (the `^persistent`
cstr-key fix; consolidation increment 3). Neither caused it -- that question is
settled twice over and does not need asking a third time.

## Repro

```turmeric
(defn just-pure [^m] [^Applicative m x : (m int)] : (m int)
  (pure 7))

(defn main [] : int
  (println (unwrap-or (just-pure (some 0)) -1))          ; want 7
  (println (unwrap-or (:: (pure 7) (Option int)) -1))    ; want 7
  0)
```

```
$ tur run p1.tur          # compiled
7
7
$ tur interpret p1.tur    # interpreted
1        <- wrong
7        <- right
```

Two things the pair isolates:

- **A concrete ascription resolves fine.** `(:: (pure 7) (Option int))` is right
  on both paths. The expected-type channel is enough when the type is there.
- **Inside the constrained generic it does not.** `m` is abstract, so there is
  nothing concrete at the `pure` call, and the interpreter keeps whatever the
  elaborator baked.

The payload is irrelevant, which is the giveaway that no real `pure` runs:

```turmeric
(defn pure-n [^m] [^Applicative m x : (m int) n : int] : (m int) (pure n))
;; interpreted: (pure-n (some 0) 7) -> 1     (pure-n (some 0) 99) -> 1
```

## It is picking one instance for everything

The clearest evidence is the sibling fixture built to distinguish two instances,
`tests/fixtures/hkt-constrained-pure-two-instances` (`107` / `207` are two
differently-tagged Applicatives):

```
expected (and compiled):  107  207
interpreted:              207  207
```

One instance answers both call sites. Same for
`hkt-constrained-continuation-dict`: `207 207` where `107 207` is wanted.

## The suite-visible symptom (from the two absorbed reports)

`tests/fixtures/hkt-constrained-byvalue-bind-pure` is the only member of the
`hkt-constrained-*` family the interpreter actually runs, so it is the one red
line this defect produces in `bash tests/run-turi.sh`:

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret \
    tests/fixtures/hkt-constrained-byvalue-bind-pure/input.tur
# prints:   1 / -1 / 1
# expected: 42 / -1 / 7
```

Line by line, which is what identifies the mechanism:

| Line | Shape | Interpreted | Why |
| --- | --- | --- | --- |
| 1 | `(bind (some 41) (fn [v] (pure (+ v 1))))` | `1`, want `42` | reaches `pure` inside a constrained callee |
| 2 | `none` short-circuit | `-1`, correct | `bind` is receiver-dispatched and never calls the continuation |
| 3 | `(just-pure (some 0))`, body `(pure 7)` | `1`, want `7` | reaches `pure` inside a constrained callee |

The middle line being right is the tell: `bind` is receiver-dispatched and
re-resolves correctly; only the two lines that reach the return-directed `pure`
are wrong. As of 2026-08-01 this is the **sole** failure in a full
`tests/run-turi.sh` run (1699 passed, 1 failed, 697 skipped).

## Why the existing fix does not cover it

`docs/archive/turi-generic-dict-dispatch-bakes-representative-instance.md` fixed
exactly this shape for **receiver**-dispatched methods: the elaborator bakes the
carrier representative into a generic body, and `gde_reresolve_method`
(`src/turi/eval.c`) re-resolves it at runtime from the concrete type pinned onto
a tyvar by the call's `abi_bindings`.

That path is gated on the receiver being a tyvar -- `dict_arg` set **and**
`abi_bindings[0].type` of kind `TY_TYVAR`. A return-directed method has no
receiver at all: `pure`'s only argument is the payload, and the class variable
appears solely in its return type. So the gate never fires and the baked
representative survives.

In the same fixture, `bind` -- receiver-dispatched -- works. That is why
`hkt-constrained-byvalue-bind-pure`'s middle line is right (`-1`, `bind` on
`none` short-circuits and never calls the continuation) while the two lines that
reach `pure` are wrong.

The compiled path solves this with dictionaries it does not share: per that
fixture's own header, the direct call routes through the callee's *dict clone*
and the lifted continuation captures the Applicative dict in its closure env, so
`pure` is a dict-slot load. The tree-walker passes no dictionaries.

This is the same family as root cause B of
`docs/archive/map-show-keyword-key-raw-int.md` -- an interpreter dispatch that
has to recover a type the compiled path carries in a dict.

## Nothing regressed -- a carve-out lifted

Worth stating plainly, because the fixture went red at a specific commit:

- `hkt-constrained-byvalue-bind-pure` was added by `48de80c9`
  ("fix(hkt): box aggregate continuation returns at the dict-dispatch carrier").
- Verified the failure is not from local work: swapping the three locally
  modified compiler files (`types.c`, `emit_module.c`, `emit_core.c`) for their
  `origin/main` versions and rebuilding produces the **identical** `1 / -1 / 1`.
- The whole `hkt-constrained-*` family diverges under the interpreter, but every
  other member contains inline-C and is PASS-skipped by `run-turi` under the
  permanent TI7 carve-out:

  ```
  SKIP hkt-constrained-continuation-dict (inline-c carve-out)
  SKIP hkt-constrained-pure-two-instances (inline-c carve-out)
  ```

  `hkt-constrained-byvalue-bind-pure` has **no inline-C block** -- it is the
  first member of the family the interpreter actually runs.

So the commit did not break the interpreter; it wrote the family's first
inline-C-free fixture and thereby made a pre-existing gap visible. The gap is
older than the fixture and wider than it: `run-turi` was reporting green on this
family only because it never executed any of it.

## PARTIALLY FIXED 2026-08-01 -- direct-call shape resolved, rank-2 forall not

Fix direction 1's first candidate landed (`gde_reresolve_return_directed`,
`src/turi/eval.c`). The mechanism was confirmed by instrumenting the call site,
and it is simpler than "consult the class variable's bind": a return-directed
call pins **nothing at all**.

```
[gde] call fn=__inst_Applicative_pure_Schema nabi=0 | frame tyvar m -> Option
```

`n_abi_bindings == 0`, because the class variable lives only in the result type
and there is no receiver argument to carry it -- so the EX_CALL gate
(`n_abi_bindings >= 1 && abi_bindings[0].kind == TY_TYVAR`) can never fire, and
`__inst_Applicative_pure_Schema` answers. But the concrete type is *already on
the frame*: `frame_record_abi` pinned `m -> Option` when the caller entered the
generic. Nothing needed computing -- only reaching.

The fix adds a fallback for the zero-binding shape that walks the frame chain
and lets the instance table pick: try each concretely-bound tyvar, keep those
that resolve to a real instance of this class, and act only when exactly one
does (two candidates is genuinely ambiguous from here, so it bails and keeps
prior behaviour). Scoped to `n_abi_bindings == 0` so a receiver-directed call
whose `abi_bindings[0]` merely failed to match keeps its exact prior dispatch.

**Fixed** -- the direct-call shape, which is this report's headline repro and
the suite's only red:

| Probe | Was | Now |
| --- | --- | --- |
| `(just-pure (some 0))` / ascribed `(pure 7)` | `1` / `7` | `7` / `7` |
| `hkt-constrained-byvalue-bind-pure` (the `run-turi` red) | `1 / -1 / 1` | `42 / -1 / 7` |

**Still open** -- the rank-2 forall shape. `hkt-constrained-pure-two-instances`
and `hkt-constrained-continuation-dict` still print `207 207` where `107 207` is
wanted. They do not call the generic directly: `make` is passed as a value into
`(defn at-t1 [g (forall [(m :: * -> *)] ...)])` and invoked as `(g (mk-t1 0))`,
so no tyvar reaches the frame for the fallback to find and it correctly
declines rather than guess. Pinning `m` across a rank-2 forall parameter is a
separate piece of work from reading a binding that is already there.

Both are inline-C carve-out PASS-skips under `run-turi`, so neither shows up in
the suite -- which is exactly the visibility trap the "Coverage note" below
warns about. They are the reason this report stays open.

## Fix directions

1. **Extend the re-resolution to the return-directed shape.** `gde_reresolve_method`
   already knows how to go from a concrete type to an instance; what is missing
   is a source for that type when there is no receiver. Two candidates, in
   order of cheapness:
   - the *caller's* pinned tyvar for the class variable -- `just-pure`'s `m` is
     bound to `Option` by the `(just-pure (some 0))` call site, and
     `frame_record_abi` already stores that substitution as a `TyvarBind` on the
     callee frame. A return-directed method should consult the class variable's
     bind rather than `abi_bindings[0]`.
   - the expected type at the call, which is what already makes the ascribed
     `(:: (pure 7) (Option int))` case work.
2. **Or make the interpreter use the dict clones the elaborator already builds.**
   The principled fix, and what would retire this whole family of divergences
   (this report, the archived receiver-dispatch one, and map-show's root cause
   B).

   Scoped out in [docs/archive/turi-dict-passing-plan.md](turi-dict-passing-plan.md),
   and it is **smaller than "a much larger change" suggests**: `make_dict_clone`
   lives in the *elaborator* (`elab_call.c`), and instrumenting it shows it fires
   on the interpreter path too -- `[dictclone] just-pure` under both `emit-c` and
   `interpret`. The dictionaries already exist in the tree turi walks; what is
   emit-only is the *lowering* (`((void**)__dict)[0]`). turi contains zero
   references to `dict_clone` and resolves `EX_DICT` from the elaboration-baked
   instance instead. So the work is "follow the clone", not "build dict passing".

## Coverage note

If this is not fixed soon, the honest interim is a `requires.*` marker on
`hkt-constrained-byvalue-bind-pure` **with a comment pointing here** -- matching
how its siblings are already carved out. That trades a red for a visible skip.
It should not be done silently: the skip is what hid the gap in the first place,
and the family currently reports green while executing none of it.

## RESOLVED 2026-08-13 -- the rank-2 forall shape

The remaining half is fixed. `hkt-constrained-pure-two-instances` and
`hkt-constrained-continuation-dict` now print `107 207` interpreted, matching
compiled.

### Cause

The report is right that "no tyvar reaches the frame for the fallback to find".
Instrumenting the call confirms it, and shows the shape precisely:

```
[rd-call] fn=at-t1 nabi=0 ...
[rd-call] fn=g     nabi=0 nargs=2  fnname=make
   param[0] x declkind=21 declhead=-  | argkind=21 arghead=T1
[rd-call] fn=__inst_Applicative_pure_T2 ...     <- the baked representative
```

Every call in the chain pins nothing, so `gde_reresolve_return_directed`'s
frame walk has nothing to walk. `make` is reached through a rank-2 `forall`
*parameter*, so there is no named generic for the elaborator to record a
call-site substitution against.

But the substitution is sitting in plain view on the same line: the callee
declares `x : (m int)` (`declkind=21` is `TY_APP`, head a tyvar, hence
`declhead=-`) and the argument's static type is `(T1 int)` -- the same `TY_APP`
with a **concrete** head. Matching the two gives `m -> T1`, and it is a
different head at each of the two call sites, which is exactly the
discrimination that was missing.

### Fix

`frame_pin_hkt_tyvars_from_args` (`src/turi/eval.c`) pins the callee's tyvar
from the call site's static argument type, and runs **only** when the call
recorded no `abi_bindings` -- so every call that pins something keeps its exact
prior dispatch, the same scoping discipline the direct-call fix used.

It is deliberately narrow: only a declared `(tyvar arg)` against a
concrete-headed `(Ctor arg)` is matched, not general structural unification.
The extra reach would be untested, and its failure mode would be dispatching to
a *wrong* instance -- strictly worse than the conservative baked-representative
answer this replaces. Same reasoning the ambiguity bail in
`gde_reresolve_return_directed` already applies.

### Coverage -- the visibility trap, closed

This report's "Coverage note" warns that the family reports green while
executing none of it. That is why the gap survived, and it applied to the fix
as much as to the bug: both rank-2 fixtures carry inline-C and are PASS-skipped
by the TI7 carve-out in `tests/run-turi.sh`, so a fix verified only against
them would have been just as invisible as the defect.

`tests/fixtures/hkt-rank2-forall-pure-two-instances` is an **inline-C-free**
restatement of `hkt-constrained-pure-two-instances`. Parametric ADTs (`defdata
B1 [a] (Mk1 :int a)`) give the same two-instance discrimination with no inline-C,
so `run-turi` actually executes it. One wrinkle worth recording: the tag has to
be a literal field set by `pure` (`(Mk1 100 x)`), not arithmetic on the payload
-- `x` is still a tyvar inside `pure`'s body, and `(+ x 100)` is a `TUR-E0006`.

Verified against a deliberately-reverted build: interpreted, it fails with
`eval: match: no arm matched`, because the wrong instance's `pure` builds a
`Mk2` value that `un-1`'s `Mk1` arm cannot match. A hard error rather than the
silent `207 207` the inline-C fixtures produce -- a better regression signal
than the shape it replaces.

### Verification

| Fixture | Compiled | Interpreted (before) | Interpreted (now) |
|---|---|---|---|
| `hkt-constrained-pure-two-instances` | `107 207` | `207 207` | `107 207` |
| `hkt-constrained-continuation-dict` | `107 207` | `207 207` | `107 207` |
| `hkt-constrained-byvalue-bind-pure` | `42 -1 7` | `42 -1 7` (fixed 2026-08-01) | `42 -1 7` |
| `hkt-rank2-forall-pure-two-instances` (new) | `107 207` | *(would not run)* | `107 207` |

`tests/run.sh`: 2593 passed, 0 failed. `tests/run-turi.sh`: 1780 passed, 0
failed, 705 skipped -- both counts up by one, which is the new fixture being
executed by both engines rather than skipped by one.

### What this does not close

Fix direction 2 -- making the interpreter use the dict clones the elaborator
already builds ([docs/archive/turi-dict-passing-plan.md](turi-dict-passing-plan.md))
-- is still the principled fix and still worth doing. Both halves of this report
are re-resolution heuristics that recover, at run time, a type the compiled path
carries in a dictionary. They are correct on every shape now known, but each was
added after a shape was found to escape the previous one, and that is the
pattern dict passing would end rather than extend.
