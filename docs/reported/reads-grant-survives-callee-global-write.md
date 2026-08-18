# The `#reads` congruence grant survives a callee's write to a frozen global

**Severity:** high (unsound -- a refinement precondition that is FALSE at the
crossing is statically "proven", and the runtime entry check is suppressed for
exactly these measures, so there is no backstop)
**Found:** 2026-08-18, researching whether a `#reads [*global*]` frame should
grant congruence at all (the open question left by
[reads-frame-cannot-name-multiple-params](../archive/reads-frame-cannot-name-multiple-params.md)).

**This is not a bug in the proposed globals feature. It is reachable today**,
with the shipped single-parameter `#reads`, by passing a mutable global as the
measure's argument.

## Summary

The C2 grant treats a `#reads w` measure as congruent when its argument is
frozen at the call site -- a live borrow proves the state cannot change, so two
occurrences denote one value. For a **local**, that holds: a direct `set!`
invalidates the hypothesis, and a callee cannot reach the local without being
handed it.

For a **global**, it does not hold. A global is reachable by name from any
callee, so it can be mutated between the guard and the crossing with **no
syntactic trace at the call site** -- and the frozen hypothesis is never
invalidated.

## Repro

```turmeric
(defstruct Wld [n : int])
(def ^mut *g* (Wld 42))

(defn wld-n [^borrow w : Wld] : int (.n w))

;; Opaque to the purity walk, but genuinely reads w's state: true iff n != 7.
(defn alive? [^borrow w : Wld e : int] #reads w : bool
  ```c
  (void)e;
  return __TUR_CNAME_wld-n__(w) != 7;
  ```)

(defn get-Pos! [^borrow w : Wld e : #refine{ x : int | (alive? w x) }] : int
  (.n w))

(defn sneaky [] : int            ;; mutates the global; nothing says so
  (set! *g* (Wld 7))
  0)

(defn run [] : int
  (let [__frz (& *g*)]           ;; frozen region over a GLOBAL
    (if (alive? *g* 0)           ;; TRUE here (n = 42)
      (do
        (sneaky)                 ;; n becomes 7 -> alive? is now FALSE
        (get-Pos! *g* 0))        ;; precondition violated AT the crossing
      -1)))

(defn main [] : int (println (run)) 0)
```

```
$ tur run --strict-refine unsound.tur
7
```

Compiles, runs, prints `7`. The crossing's precondition `(alive? w x)` is
false when it is taken.

## Why there is no backstop

The usual soundness argument for this grant is "it elides the caller-side
crossing check, never the callee's own entry check". That argument does not
apply here: `rt_pred_reads_measure` (`elab_fns.c`) exists specifically to
**skip the runtime entry contract** for a `#reads` measure -- the contract is
unemittable for an impure measure. So the static proof is the only check, and
it is wrong.

`--enable=checked-reads` does not help (verified -- still prints `7`). R2 keys
on evidence that the **measure's own body** reads a mutable global; here the
measure reads only its parameter, and it is the *caller's callee* that writes
the global. R2 is looking in the wrong place for this shape.

## Controls

Each of these was run, so the finding is not an artifact of the setup:

| variant | result | reading |
|---|---|---|
| the repro above | prints `7` | precondition false at the crossing |
| same, **no** frozen region | TUR-W0372 | the grant is what carries the proof |
| same, `--enable=checked-reads` | prints `7` | R2 does not cover this shape |
| direct `(set! *g* ...)` between guard and crossing | TUR-W0372 | in-function `set!` DOES invalidate |
| local + `(set! w ...)` between guard and crossing | TUR-W0372 | locals invalidate correctly |
| local + callee `bump` taking `^mut w` | prints `42` | the callee cannot reach the caller's local |

The last two rows are why this is specific to globals: for a local the
mutation is either visible in-function (and invalidates) or cannot happen.

## Root cause

The frozen set is a list of **names** (`Enc.frozen_names`, populated from
`ScopeBorrow`s in `elab_fns.c`). `enc_reads_one_arg_frozen` resolves the
call-site argument to a name and asks whether that name is in the set. Nothing
in that path distinguishes a name bound to a local from a name bound to a
mutable global, and nothing re-examines the set after an intervening call.

## Fix directions

1. **Narrow (correct, cheap):** refuse the grant when the `#reads` argument
   resolves to a mutable global binding. The encoder is name/form-based, so
   the global-ness has to be threaded in -- either alongside `frozen_names`
   (a parallel `is_global[]`) or by simply not publishing a mutable global
   into the frozen set in the first place, which is the smaller change and
   fails safe.
2. **Principled (better, and the machinery already exists):** invalidate a
   frozen global at any intervening call whose callee may write it.
   `wf_fn_writes_global` (`elab_fns.c:1670`) already computes exactly this,
   with a per-global `WgSet`, a fixed point over callees, and an overflow
   flag -- the same analysis `#writes`'s G1 work uses. A call with `WG_YES`
   naming a frozen global (or `WG_UNKNOWN` / overflow) drops that global from
   the hypothesis, which is the direct analogue of the in-function `set!`
   invalidation that already works.

(1) is a strict subset of (2)'s behaviour and could ship first.

## Consequence for the globals feature

This settles the open question from the multi-param work: a `#reads [*cache*]`
entry **must not** grant congruence on the strength of a frozen region alone.
Under fix (1) it would grant nothing at all; under fix (2) it could grant
congruence only when no intervening call may write it -- which is a real
guarantee, and the only one worth having.

Either way the global entry's *purpose* differs from the parameter entry's,
which is one more argument for the parallel-field representation rather than a
shared mask.
