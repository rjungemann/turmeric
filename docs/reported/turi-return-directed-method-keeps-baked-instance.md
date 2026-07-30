# `--interpret` keeps the baked instance for a RETURN-directed class method

**Severity:** medium. Silent wrong answers under `--interpret` / `tur repl` for
any constrained generic that calls a return-directed class method (`pure`,
`default-of`, ...). The compiled path is correct, so this is an
interpreter/compiled divergence, not a miscompile.

**Status:** open. Found 2026-07-30 while checking whether
`hkt-constrained-byvalue-bind-pure` (red under `run-turi`) was caused by local
changes. It is not -- see [Nothing regressed](#nothing-regressed-a-carve-out-lifted).

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
2. **Or give the interpreter real dictionaries** for constrained generics. The
   principled fix, and what would retire this whole family of divergences
   (this report, the archived receiver-dispatch one, and map-show's root cause
   B), but a much larger change.

## Coverage note

If this is not fixed soon, the honest interim is a `requires.*` marker on
`hkt-constrained-byvalue-bind-pure` **with a comment pointing here** -- matching
how its siblings are already carved out. That trades a red for a visible skip.
It should not be done silently: the skip is what hid the gap in the first place,
and the family currently reports green while executing none of it.
