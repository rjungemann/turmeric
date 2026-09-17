---
title: A `let` inside a self-tail-recursive body drops the carrier-to-by-value bridge
category: Reported
description: "emit_tail's inline tail-position `let` arm assigned emit_value's result straight into a by-value-typed local instead of routing it through emit_carrier_bridge, so `struct x = <int64_t>;` reached cc. Both the TCO and the carrier producer were required."
---

# A `let` inside a self-tail-recursive body drops the carrier-to-by-value bridge

**RESOLVED 2026-09-16.** Root cause as filed, and the fix is the report's own
structural direction rather than its fix direction 1: the decision is now one
function, `emit_let_init_carrier_bridge_type` (`src/compiler/emit_expr.c`),
called from all three sites. It had **two byte-identical copies** already --
`emit_let_value` and `emit_letrec_value` -- which is the strongest argument
against re-typing it a third time in `emit_tail`. The arm keeps its TCO
back-edge; nothing loses the optimization. Pinned by
`tests/fixtures/tail-recursive-let-carrier-bridge`, which carries both controls
(recursive call out of tail position, and the `let` hoisted into a
non-recursive helper) alongside the regression, because either one alone makes
the defect disappear.

**Spice-side follow-up: closed, by deciding NOT to do it**
(turmeric-spices#75). This note used to say that
`turmeric-spices/spices/nng/tests/nng/pubsub_test.tur`'s `send-until-received?`
could inline its receive back into the recursive body once the fix landed. It
can -- that shape compiles now -- but it should not. `recv-str=?` has twelve
call sites across that file and `msg_test.tur`, so inlining a twelve-caller
helper into one of them, to re-prove something
`tests/fixtures/tail-recursive-let-carrier-bridge` already pins here, would be
worse code for redundant coverage.

The delegation was good factoring independent of this defect; only the comment
claiming it was load-bearing was stale, and that is what was removed. Worth
recording because "undo the workaround" is the obvious reading of a fixed
report, and it is the wrong one here.

**Severity: medium.** Not a miscompile -- the emitted C is rejected by the C
compiler, so the failure is loud. But it is rejected with a message about a
generated identifier (`initializing 'tur_adt_Result__cstr__int' ... with an
expression of incompatible type 'int64_t'`), which names neither the Turmeric
source line nor the construct, and the same code compiles fine the moment the
recursion stops being a tail call. Writing a retry loop over a fallible
operation -- poll, re-send, re-try -- is an ordinary thing to want, and this
rejects the obvious spelling of it.

**Status when filed: open.** Found writing the `nng` spice's pub/sub slow-joiner retry
helper (`spices/nng/tests/nng/pubsub_test.tur`, which now delegates the receive
to a non-recursive helper to sidestep it, with a pointer back here).

## Repro

```turmeric
(defmodule repro/a (export)

(defn make [n : int] : (Result cstr int)
  ```c
  if (n <= 0) return tur_err_int(1);
  return tur_ok_ptr((void *)"x");
  ```)

(defn go [n : int] : bool
  (if (<= n 0)
    false
    (let [r (make n)]          ;; <-- binds a carrier-returning Result...
      (if (ok? r)
        true
        (go (- n 1))))))       ;; <-- ...in a SELF-TAIL-RECURSIVE body

(defn main [] : int
  (if (go 3) (println "ok") (println "no"))
  0)
)
```

```
$ tur build a.tur -o a
a_tur.c:8060:43: error: initializing 'tur_adt_Result__cstr__int'
  (aka 'struct tur_adt_Result__cstr__int') with an expression of
  incompatible type 'int64_t' (aka 'long long')
```

Both ingredients are required. Each of these compiles and runs:

- **Delegate the receive.** Move the `let` into a non-recursive helper and call
  it from `go`. (This is the workaround the nng spice uses.)
- **Take the recursive call out of tail position.** `(not (not (go (- n 1))))`
  compiles -- same `let`, same types, same function.

## Root cause

`emit_tail` (`src/compiler/emit_fns.c:718`) has its own inline `EX_LET` /
`EX_LETREC` arm for a tail-position `let`, reached only for functions the TCO
pass flagged. It emits each binding as:

```c
char *iv = emit_value(ctx, body, e->as.let_.bindings[i].init);
buf_printf(body, "%s %s = %s;\n", emit_type_c_name(ctx, b->type), bn, iv);
```

-- the binding's *by-value* C type on the left, the init's raw emitted value on
the right, with nothing in between. When the init is a producer whose C return
is the uniform `int64_t` carrier (an inline-C body declared `: (Result T E)`,
among others) and the binding's type is a by-value monomorph, the assignment is
`struct = int64_t`.

The non-TCO path goes through `emit_let_value` (`src/compiler/emit_expr.c:2859`)
and its `emit_carrier_bridge` (`src/compiler/emit_core.c:5276`), which emits the
three-line unbox the by-value side needs:

```c
int64_t __t278 = (int64_t)(intptr_t)(__ps_277);
tur_adt_Result__cstr__int __t279 = (*(tur_adt_Result__cstr__int *)(intptr_t)(__t278));
if (__t278) free((void *)(intptr_t)(__t278));
tur_adt_Result__cstr__int r_1610 = __t279;
```

versus what the TCO arm emits for the identical source:

```c
int64_t __ps_275 = (repro__a__make(n));
tur_adt_Result__cstr__int r_1610 = __ps_275;   /* <-- no bridge */
```

The arm is already known to duplicate `emit_let_value`'s per-binding
bookkeeping -- the comment immediately below the assignment says so, about the
`any` drop list:

> this arm emits a tail-position `let` INLINE rather than through
> `emit_let_value`, so the `any` drop bookkeeping that lives there has to be
> repeated.

The carrier bridge is the second piece of that bookkeeping, and it was not
repeated. This is the hybrid carrier/by-value ABI seam that
`docs/upcoming/end-to-end-monomorphization-plan.md` exists to remove; until it
is gone, every site that hand-rolls a binding assignment has to ask the bridge
question.

## Fix directions

Route the tail-position binding's init through `emit_carrier_bridge` the way
`emit_let_value` does, rather than assigning `emit_value`'s result straight into
a by-value-typed local -- a targeted change inside the `EX_LET` arm at
`emit_fns.c:718-731`.

The structural fix is the one the arm's own comment implies: a shared
"emit one let binding" helper that both `emit_let_value` and `emit_tail` call,
so a third piece of per-binding bookkeeping does not have to be discovered a
third time by a spice author.

A fixture belongs in `tests/fixtures/` either way -- the repro above is small
and has no dependencies.
