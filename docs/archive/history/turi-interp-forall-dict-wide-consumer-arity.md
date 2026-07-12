---
title: Interpreter drops the implicit Functor dict for wide van-Laarhoven consumers
category: Reported
description: Under --interpret, a lens consumer taking a forall-typed lens param
  and dispatching a ^Functor constraint failed with an arity mismatch because
  the tree-walker did not account for the implicit dictionary actual the
  compiled forall-dict-pass path prepends. RESOLVED (2026-07-06) -- the driver's
  poly-call apply now skips the redundant leading dict actual(s) (the
  interpreter dispatches typeclass methods by runtime value) and binds only the
  callee's declared value params.
---

# Interpreter drops the implicit Functor dict for wide van-Laarhoven consumers

**Status:** RESOLVED (2026-07-06). The tree-walking interpreter's poly-call
apply path now skips the implicit dictionary actual(s) a call through a rank-2
`forall` param carries. See the Resolution section at the end.

**Summary:** Under `--interpret`, a lens consumer that takes a `forall`-typed
lens param and dispatches a `^Functor` constraint fails with an arity mismatch,
because the tree-walking interpreter does not thread the implicit dictionary
argument the compiled by-value HKT path passes. Affects the three
`van-laarhoven-lens-wide-consumer-{clone,forward,resolve}` fixtures
(`requires.interp`). **Severity: low** (interpreter-only; the compiled path --
the primary target and the `expected.stdout` these fixtures also carry -- is
correct).

## Repro

```sh
./build/tur --interpret --dump-mono-specs \
  tests/fixtures/van-laarhoven-lens-wide-consumer-resolve/input.tur
# tur: eval: arity mismatch: point-x expects 2 args, got 3
```

`point-x` is `(defn point-x [^f] [^Functor f g : (-> int (f int)) s : Point] ...)`.
The compiled path passes the `^Functor` dictionary as a hidden argument (3
actuals: dict, g, s); the interpreter counts only the two source-visible value
params (g, s) and rejects the dict-carrying call.

## Root cause

The interpreter (`src/turi/eval.c`) applies these consumers without the
implicit-dictionary calling convention the compiled forall-dict-pass lowering
uses. It sees the callee's declared arity (source value params) and the caller
supplying an extra dict slot, and raises `arity mismatch` instead of binding the
dict param. This is the interpreter-side counterpart of the compiled
`forall-dict-pass` work (#607/#611/#613) and the open compiled-path report
`docs/reported/forall-dict-direct-applied-nested-lambda-dispatch.md`.

## Fix directions

Teach the interpreter's apply path the same implicit-`^Functor`/`^Show`
dictionary convention: when a callee declares constraint params (`^Class v`),
accept and bind the leading dictionary actual(s) the caller passes, rather than
counting only the trailing value params. Until then these three interpreter
fixtures stay red while their compiled `expected.stdout` passes.

---

## Resolution (2026-07-06)

Diagnosis refined the "bind the dict" fix direction into a simpler, correct one:
the tree-walker does not *need* the runtime dictionary at all. Unlike the
compiled path -- which binds each prepended dict actual to a dict-clone param and
dispatches through it -- the interpreter dispatches a typeclass method on the
**runtime value** of its receiver (`gde_reresolve_method_by_value`,
`src/turi/eval.c`). A direct call to `point-x` already runs correctly under
`--interpret` for exactly this reason; only the call *through* a rank-2 `forall`
param fails, because elaboration prepends the implicit dict actual(s) there
(the `EX_CALL` is flagged `is_poly_call`), and the callee's `FnDef` has no
parameter slot for them.

**Change** (`src/turi/eval.c`, driver `DK_CALL_ARG` apply path): when
`top->expr->as.call_.is_poly_call` and the collected actual count exceeds the
callee's declared value-param count, treat the leading `n_args - effective_params`
actuals as the implicit dictionaries, skip them (`arg_base`), run the arity check
against the remaining actuals, and bind only the callee's declared value params
from `acc[arg_base ..]`. Non-poly calls (`is_poly_call == false`) and poly calls
that carry no dicts (`n_args == effective_params`) are unchanged -- `arg_base`
stays 0 and the binding loop is identical to before.

Why skipping is correct rather than lossy: the skipped actuals are precisely the
Functor/Show dictionaries the compiled ABI threads; the interpreter re-derives
the same instance from the receiver value at each method call, so the callee body
runs identically to a direct call. Verified: the `-clone` fixture prints the
same eight values under `--interpret` as under the compiled path (`tur run`) and
as its `expected.stdout` (99, 4, 3, 88, 30, 4, 3, 40), exercising `set`/`over`
through both `point-x` and `point-y`.

**Fixtures.** `-clone` and `-forward` assert real runtime output and now pass
under `--interpret` (they crashed with the exact `arity mismatch: point-x
expects 2 args, got 3` before the fix). `-resolve`'s `expected.stdout` is the
compile-time `--dump-mono-specs` dump (its `main` prints nothing at runtime);
the tree-walker has no monomorphization pass, so it cannot produce those lines.
It gains a `requires.compiled` marker so `tests/run-turi.sh` skips the
pure-interpret run -- `tests/run.sh` still asserts it via the single-process
`tur run` capture path (`requires.interp`).

**Tests.** `bash tests/run-turi.sh` (every fixture under `--interpret`): 1428
passed, 0 failed. `bash tests/run.sh` (compiled): 1951 passed, 0 failed.

Not pursued: binding the dict actuals to synthetic params. That would only
matter if the interpreter dispatched through a runtime dict, which it does not;
value-directed dispatch already covers every shape these consumers produce. The
sibling compiled-path report
`docs/reported/forall-dict-direct-applied-nested-lambda-dispatch.md` is a
distinct lowering boundary and is unaffected.
