---
title: A `bt-scope` in a non-main defn emits a call to an undeclared closure temp
category: Reported
description: The CPS lowering of a function whose body is a `bt-scope` over a thunk that calls another user function emits `bt_hyscope(... _un_unborrowc_unNNNN_NNNN)` without ever declaring that temp, so the emitted C does not compile. Flag-independent; not a regions bug.
---

# `bt-scope` in a non-main defn: `cps->direct` call names an undeclared temp

**Severity: medium.** A hard `cc` failure, not a wrong answer -- but it makes
the natural spelling of a bracketed helper (`(defn round [n] (bt-scope (fn []
...))))`) unusable, which is exactly the spelling `bt-scope` exists for. Every
in-tree caller happens to sit in `main` or pass a thunk that calls nothing, so
nothing caught it.

Found while wiring RM3 R4 (`docs/upcoming/regions-plan.md`), whose benchmark and
fixtures both wanted that spelling. Independent of `--enable=regions`: it
reproduces identically with the flag off.

## Repro

```turmeric
(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (+ acc n))))

(defn one-round [n : int] : int
  (bt-scope (fn [] (build n 0))))          ;; <- in a NON-main defn

(defn main [] : int (println (one-round 3)) 0)
```

```
$ ./build/tur run r3.tur
r3_tur.c:7012:52: error: '_un_unborrowc_un1444_1445' undeclared (first use in this function)
 7012 |     int64_t __t182 = bt_hyscope((int64_t)(intptr_t)_un_unborrowc_un1444_1445); /* cps->direct */
tur: cc invocation failed (status 256)
```

Three neighbours that all work, which is what narrows it:

- the same `bt-scope` inlined into `main` -- fine;
- `(bt-scope (fn [] (+ n 1)))` in a non-main defn (thunk calls nothing) -- fine;
- `bt-scope` in a non-main defn under a self-recursive caller -- same failure,
  so the caller is not the variable.

The trigger is: **a `bt-scope` in a non-main `defn` whose thunk body calls
another user function.**

## Emitted C

```c
static int64_t one_hyround__cps(int64_t n, DK *__kont) {
    int64_t __t182 = bt_hyscope((int64_t)(intptr_t)_un_unborrowc_un1444_1445); /* cps->direct */
    return dk_run(__kont, (intptr_t)(__t182));
}
```

The function is CPS-lowered, and the `cps->direct` bridge emits the CALL but not
the statements that build its `^fat` closure argument -- `_un_unborrowc_un*` is
the unborrow temp the direct path would have declared just above. So the
argument's emission is dropped, not mis-scoped: nothing in the TU declares that
name.

Root cause is therefore in the CPS IR's direct-call bridge (`src/compiler/
emit_cps_ir.c`, the site that writes the `/* cps->direct */` comment), which
re-emits the call text without re-emitting the operand statements the direct
emitter had put in the enclosing body.

## Fix directions

Either emit the operand statements into the CPS block alongside the call, or
decline to CPS-lower a call whose arguments carry pending statement-level
setup and keep it on the direct path. The second is the smaller change and
matches what the bridge's name claims it is doing.

## Workaround in the meantime

Put the `bt-scope` in `main`, or give the thunk a body that calls nothing.
`tests/fixtures/region-scope-value-survives`,
`tests/fixtures/region-scope-escape-refused` and
`benchmarks/bench-regions-subst.tur` all carry that workaround with a comment
pointing here; when this is fixed they can be written the natural way.
