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

## Fix directions

1. **Add the `EX_UNION_INJECT` arm**, recursing into the injected value, beside
   the `EX_ASCRIBE` arm. One line, and it makes the walk say what the
   neighbouring comment already claims.
2. **Audit the walk for the other Saffron nodes while there.** `EX_DYN_OP`,
   `EX_DYN_CALL` and `EX_DYN_FIELD` have no arms either (the `EX_DYNVAR_BINDING`
   arm is an unrelated node -- dynamic *variables*). A control op nested inside
   a dynamic operator's argument -- `(+ 1 (call/cc f))` in a Saffron file --
   would be missed the same way. **Not yet measured**; it is the obvious next
   probe and should be confirmed before the arms are written, not assumed.

Direction 1 fixes the filed repro. Direction 2 is the same fix applied to the
rest of the family and should land with it, with a fixture per shape that
actually reproduces.

## Not this bug

`fn_sig_ok` and `slot_box_ty` handle `any` correctly; the gate is not reached.
Nor is this the `any`-param case, which was fixed separately and has its own
comment in `emit_cps_ir.c`. And `call/cc` itself is fine -- with the return
annotated it compiles and prints `42` in a Saffron file exactly as in Turmeric.
