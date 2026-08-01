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

   Scoped out in [docs/upcoming/turi-dict-passing-plan.md](../upcoming/turi-dict-passing-plan.md),
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
