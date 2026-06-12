# Compiled path: STM `or-else` branches are no-op stubs (nested `stm` not emitted)

> **Status: FIXED.** The `EX_STM` codegen arm (`src/compiler/emit_expr.c`) now
> emits its body for real instead of a no-op stub: statements in order, the last
> as the block's value, with a `__tur_stm_should_retry` short-circuit after a
> `check`/`retry` (so a speculative write following an aborted guard is not
> buffered) -- mirroring the interpreter's `EX_STM`. `EX_ATOMICALLY` still
> inlines its direct `stm` body unchanged (snapshot-neutral); the standalone arm
> is reached only for nested `stm` blocks such as `or-else` branches. Verified
> `tur run` and `tur --interpret` agree, including the short-circuit and the
> stm1-succeeds-so-stm2-skipped cases. Guarded by `tests/fixtures/stm-or-else/`.
> No `expected.c` snapshot changed (none nested an `stm` block).

**Summary:** On the compiled path, `(or-else stm1 stm2)` does nothing useful:
both `stm1` and `stm2` are emitted as no-op stubs, so their `tvar/read`/
`tvar/write`/`tvar/cas`/`check` operations never run and the `or-else`
expression always evaluates to `NULL`/`0`/`false`. The interpreter
(`tur --interpret`, Phase TI4) handles `or-else` correctly.

**Severity:** High -- `or-else`, one of the headline composable-STM primitives,
is completely non-functional when compiled (silent wrong answer, not a build
error).

## Minimal repro

```turmeric
(defn main [] : int
  (let [tv (tvar/new 10)]
    ;; stm1 requests retry (check false); or-else should fall back to stm2,
    ;; which sets tv 10->23 and returns true.
    (println (atomically (stm (or-else (stm (check false) (tvar/cas tv 0 0))
                                       (stm (tvar/cas tv 10 23))))))
    (println (atomically (stm (tvar/cas tv 23 23)))))   ; did stm2 commit 23?
  0)
```

- `tur run`        -> `false` / `false`  (wrong: stm2 never ran)
- `tur --interpret` -> `true` / `true`   (correct: stm2 ran, tv committed 23)

## Root cause

Only `EX_ATOMICALLY` knows how to emit an `stm` block: its codegen
(`src/compiler/emit_expr.c` `case EX_ATOMICALLY`) reaches *into* its direct
`EX_STM` child and emits each body expression inline inside the transaction
loop. The standalone `EX_STM` case is a stub:

```c
case EX_STM: {
    /* The EX_ATOMICALLY case handles the emission, so we should not reach here */
    ... emits: void *<tmp> = NULL;
}
```

But `or-else`'s two arguments are themselves `EX_STM` nodes
(`elab_or_else` requires `stm1->kind == EX_STM` and `stm2->kind == EX_STM`),
and `EX_OR_ELSE` codegen emits them with `emit_value`, which dispatches to the
`EX_STM` **stub**. The emitted C is:

```c
/* or-else */
bool __or_else_retry_before = tur_stm_current_tx()->retry_requested;
/* STM block (should be inside atomically) */ void *__t28 = NULL;   // stm1: NO-OP
__t27 = __t28;
if (!__or_else_retry_before && tur_stm_current_tx()->retry_requested) {
    tur_stm_current_tx()->retry_requested = false;
    /* STM block (should be inside atomically) */ void *__t29 = NULL; // stm2: NO-OP
    __t27 = __t29;
}
```

So neither branch's body is emitted. `check`/`cas`/`read`/`write` inside the
branches never execute; `__or_else_retry_before`/`retry_requested` never change;
the result is always `NULL`. (The `EX_OR_ELSE` codegen also carries its own
"simplified ... doesn't properly handle the retry semantics" caveat.)

## Proposed fix directions

Make `EX_STM` emittable as an inline block instead of a stub: emit each body
expression as a statement, the last as a value, returning that value -- the
same loop the `EX_ATOMICALLY` case runs over `stm_.body`, factored into a
shared helper. Then `EX_OR_ELSE` (and any other context that nests an `stm`
block) emits real work. `EX_OR_ELSE` should additionally short-circuit stm1 on
a retry request (mirroring the interpreter, where a `check`/`retry` aborts the
rest of the block before `or-else` falls back to stm2).

A symmetric interpreter detail already exists: `src/turi/eval.c` `case EX_STM`
runs the body and short-circuits when `retry_requested`/`aborted` is set;
`case EX_OR_ELSE` saves the pre-retry flag, runs stm1, and runs stm2 only if
stm1 newly requested a retry. The compiled emission should match that shape.

## How to validate a fix

The repro above should print `true` / `true` under `tur run`, matching
`tur --interpret`. Add a compiled fixture (`tests/fixtures/stm-or-else/` with
`input.tur` + `expected.stdout`) and confirm `bash tests/run.sh` is green, then
add it to the turi allowlist once the harness interprets (TI8).

## Related

- [docs/reported/stm-tvar-cas-swap-modify-compiled-path-broken.md](stm-tvar-cas-swap-modify-compiled-path-broken.md)
  -- the cas/swap link failures and modify stub, fixed alongside discovering
  this one.
