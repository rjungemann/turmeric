---
title: "`cps_directly_uses_control` has no `EX_UNION_INJECT` arm, so a control op under an `any` return ICEs"
category: Reported
description: "(defn go [] (call/cc (fn [k] (k 42)))) in a Saffron file aborts with `tur: emit: EX_CALLCC reached the direct emitter`. The any return wraps the body in EX_UNION_INJECT; the coloring walk has no arm for it, falls to `default: return false`, and the function is never a CPS candidate. Annotating the return to `: int` fixes it. One missing switch arm, next to the EX_ASCRIBE arm that exists for exactly this reason."
---

# The CPS coloring walk does not see through an `any` return widen

**Severity: medium.** A hard compiler abort (`tur: emit: ...`), so it is loud
and nothing miscompiles. What it blocks is `call/cc` -- and by the same route
every other delimited-control op -- in any function whose return type is `any`,
which in a `#lang saffron` file is *every unannotated function*.

Found while probing `^linear` for saffron-lang-plan D7. The `^linear` question
turned out to be unrelated (linearity checks fine in Saffron); this fell out of
the same probe and is the real defect.

## Repro

```turmeric
#lang saffron
(defn go [] (call/cc (fn [k] (k 42))))
(defn main [] (println (cast (go) int)) 0)
```

```
$ tur run cc-saf.tur
tur: emit: EX_CALLCC reached the direct emitter (should be handled by the CT-IR backend)
```

Two one-character variations both fix it, which is what localises the bug:

```turmeric
#lang saffron
(defn go [] : int (call/cc (fn [k] (k 42))))   ;; annotate the return => 42
```

```turmeric
(defn go [] : int (call/cc (fn [k] (k 42))))   ;; plain Turmeric => 42
```

So the trigger is the **`any` return type**, not the dialect and not `call/cc`.

## Root cause -- measured, not read

Probes at three points, comparing the ICEing and working variants of the same
program:

| probe | `any` return | `: int` return |
| --- | --- | --- |
| `fn_sig_ok` called for `go`? | **never** | yes (`slot=1`, admitted) |
| `fd->cps_colored` | **0** | 1 |
| `go`'s body `Expr` kind | **100 = `EX_UNION_INJECT`** | 53 = `EX_CALLCC` |

The signature gate was the first suspect and is innocent -- `slot_box_ty`
explicitly admits `TY_ANY` (`emit_cps_ir.c`, from
`perform-in-fn-with-any-param-has-no-cps-lowering`), and the gate is never even
reached.

The actual chain is one step earlier. An `any` return wraps the body in a
return-position widen, `EX_UNION_INJECT`. The coloring seed is

```c
nodes[i].colored = cps_directly_uses_control(nodes[i].fd->body);
```

and `cps_directly_uses_control` (`src/passes/cps.c`) has **no arm for
`EX_UNION_INJECT`**. It falls to `default: return false`. So:

1. `go` is not coloured.
2. `cps_color_program`'s candidate loop is gated on `fd->cps_colored`, so `go`
   never becomes a CPS candidate and `fn_sig_ok` is never consulted.
3. The `EX_CALLCC` in its body reaches the direct emitter, which has no
   lowering for one, and aborts.

The walk already handles precisely this situation one node over:

```c
case EX_ASCRIBE:
    /* Ascription is erased at codegen; seed on a control op that is
     * only reachable through (:: <control-op> T). */
    return cps_directly_uses_control(e->as.ascribe_.inner);
```

`EX_UNION_INJECT` is the same shape -- a wrapper whose child carries the
control op -- and wants the same treatment.

This is the same *class* as the already-fixed `expr_subtree_has_inline_c` gap
(`docs/archive/saffron-any-return-defeats-the-frame-box-rule.md`): a structural
walk with no arm for a node Saffron introduces, silently taking the
conservative default and skipping the function. That report's own lesson was
that reading the control flow produced a plausible wrong answer twice; hence
the probe table above rather than a prose argument.

## PARTIALLY FIXED 2026-09-08 -- and the second blocker, measured

The coloring arms are landed (`EX_UNION_INJECT`, `EX_DYN_OP`, `EX_DYN_CALL`,
`EX_DYN_FIELD`, in `cps_directly_uses_control`). **The repro still ICEs**, and
the arms are still correct: they were necessary, not sufficient.

Direction 2 is now measured rather than suspected. Three of the four shapes
reproduce; all abort identically:

```turmeric
#lang saffron
(defn go [] (call/cc (fn [k] (k 42))))                  ; EX_UNION_INJECT
(defn go [x] : int (+ x (call/cc (fn [k] (k 41)))))     ; EX_DYN_OP
(defn apply2 [f] : int (f (call/cc (fn [k] (k 41)))))   ; EX_DYN_CALL
```

`EX_DYN_FIELD` has **no** repro: a dyn field's only child is its receiver, and
every attempt to hand it an `any`-typed control op there was refused earlier by
the field resolver ("no typeclass method found for 'n'"). Its arm is by
construction; do not record it as a reproduced shape.

With the arms in, `TUR_TRACE_EVICT=1` moves the failure one stage and names it:

```
before:  (no line at all -- never coloured, never a candidate)
after:   [EVICT] BODY-UNSUPPORTED  eff=0 go  unsupported form: EX_#100
```

So the function is now coloured, reaches `fn_sig_ok`, and PASSES it
(`rt=40 slot=0 box=1` -- `slot_box_ty` admits `TY_ANY`, as its comment says).
It is then evicted by the **CPS IR lowering**, which has no lowering for an
`EX_UNION_INJECT` whose operand uses control.

`cps_ir.c` does handle the node -- but only as a DELEGATABLE one:

```c
case EX_UNION_INJECT:
    return is_atomic(e->as.union_inject_.value)
        || !operand_uses_control(e->as.union_inject_.value);
```

That arm was added for the control-FREE case (its comment: "why `(with-any 3)`
in main -- a widen, nothing more -- took the whole program off the DK
backend"). A widen over a `call/cc` fails `operand_uses_control`, correctly:
delegating it would direct-emit a control op inside a CPS function.

### Two routes were evaluated, neither is small

1. **Lower the widen as a new CPS primitive** -- `atomize` the operand into a
   pending let, emit a `CT_LETPRIM`, and teach `emit_cps_ir.c` to emit it. The
   work is not the plumbing but the widen's own semantics: `EX_UNION_INJECT`
   carries a tag index, a target type, and the `frame_box` by-value/malloc
   decision, all of which the direct emitter currently owns.
2. **Hoist the control op out** so the existing delegation rule applies --
   rewrite `(union-inject (call/cc ...))` to `(let [t (call/cc ...)]
   (union-inject t))`, making the operand atomic. Elegant, and it reuses proven
   machinery -- but it needs an `Expr` that refers to a CPS-bound `CVar`, and
   **no such synthesis exists**: `CT_LETRAW` stores a raw `Expr *` and CVars are
   IR-level. That plumbing would have to be built.

Route 2 is the more attractive shape if the CVar-to-Expr gap can be bridged,
because it adds no new emission path. Neither was attempted: a half-done
backend change here produces a wrong answer rather than a loud one, which is
the trap the sibling seam fix
(`docs/archive/saffron-unannotated-param-container-cast-panics.md`) fell into
on its first attempt.

## Fix directions

1. ~~Add the `EX_UNION_INJECT` arm~~ -- **DONE**, along with the three dynamic
   nodes. Necessary, and on its own it only moves the failure.
2. **Lower a control-bearing widen in the CPS IR.** This is the remaining work;
   see the two routes above. Route 2 (hoist) first if the CVar-to-Expr gap can
   be bridged, since it adds no new emission path and the widen keeps its one
   owner.

Until 2 lands the repro still aborts -- but it now aborts with a named trace
category instead of vanishing before the candidate loop, which is what made the
first diagnosis take three probes.

## Not this bug

`fn_sig_ok` and `slot_box_ty` handle `any` correctly; the gate is not reached.
Nor is this the `any`-param case, which was fixed separately and has its own
comment in `emit_cps_ir.c`. And `call/cc` itself is fine -- with the return
annotated it compiles and prints `42` in a Saffron file exactly as in Turmeric.
