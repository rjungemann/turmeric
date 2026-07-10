---
title: "U0 -- Inventory + oracle fixtures for the CPS backend unification"
status: landed
parent: cps-backend-unification-plan.md
description: The U0 phase of the CPS backend unification. Enumerates every program shape emit_cps.c currently owns and pins each to a direct-vs-CPS value-equality oracle fixture pair. No compiler code change -- this phase only builds the regression net that guards U1-U7.
---

# U0 -- Inventory + oracle fixtures

This is the first phase (**U0**) of
[cps-backend-unification-plan.md](cps-backend-unification-plan.md). Its job is
purely to build the regression net that the porting phases (U1-U7) lean on. It
makes **no compiler code change**.

## What U0 delivers

1. A complete inventory of the program shapes `emit_cps.c` currently owns,
   each mapped to its form-presence gate (`file:line`).
2. A **direct-vs-CPS value-equality oracle** for every shape: a twin pair of
   fixtures that run the *same* program under *both* backends and pin the
   *same* `expected.stdout`.

### Why a twin pair

For the delimited-control family, both backends produce the same value today:

- the **default** backend runs the whole program through `emit_cps.c`;
- **`--enable=cps-backend`** runs colored functions through the CT-IR backend
  (`emit_cps_ir.c`), which currently *falls back* to `emit_cps.c` for these
  shapes.

Each oracle is therefore a pair:

| Fixture | `flags` | Path exercised today |
| --- | --- | --- |
| `cps-oracle-<shape>` | (none) | `emit_cps.c` whole-program transform |
| `cps-oracle-<shape>-cps` | `--enable=cps-backend` | CT-IR backend -> falls back to `emit_cps.c` |

Both twins share one byte-identical `input.tur` and one `expected.stdout`. The
invariant every port phase must preserve is **direct == cps**: as U1-U5 teach
the CT-IR backend to emit each family itself, the `-cps` twin's *emitted C*
changes but its *value* must not. A divergence turns exactly one twin red and
points at the family whose port regressed, against its still-green direct twin.

All 24 fixtures were confirmed green (leak detection on the compile path):

```
TUR_TEST_FILTER='^cps-oracle-' bash tests/run.sh
# summary: 24 passed, 0 failed
```

## The inventory: what `emit_cps.c` owns

`emit_cps.c` claims a function by *syntactic form presence*. Each row below is
one such gate, with the surface forms it captures and the oracle(s) that pin
its value.

### 1. Base delimited control -- `reset` / `shift` / `shift0`

- **Gate:** `emit_cps_program_uses_delimited` (`src/compiler/emit_cps.c:85`).
- **Runtime prelude:** `emit_cps_runtime_prelude` (`emit_cps.c:1883`), the DK
  multi-prompt machine (`dk_run`/`dk_shift`/`dk_shift0`/`dk_invoke`/...).
- **Semantics note:** the base `shift`/`shift0` here is *abortive* --
  `(shift receiver value)` captures the delimited continuation, discards it,
  and calls `receiver(value)`; the reset yields that result. `shift0` differs
  only in not reinstalling the delimiting prompt on capture.

| Oracle pair | Program shape | Value |
| --- | --- | --- |
| `cps-oracle-reset-shift` | colored fn reached from `reset`/`shift`; cps->cps + cps->direct calls threading the continuation | `18` |
| `cps-oracle-shift0` | nested `reset`s; `shift0` delimiter behaviour | `105` |
| `cps-oracle-shift-capture-body` | a `shift` body that references an enclosing local (LH_SHIFT_BODY env capture) | `105` |

### 2. Undelimited control -- `call/cc` / `escape`

- **Gate:** `emit_cps_program_uses_callcc` (`src/compiler/emit_cps.c:140`).
- **Runtime prelude:** `emit_cps_callcc_prelude` (`emit_cps.c:426`).
- **Semantics note:** unannotated `k` defaults to the one-shot escape flavor;
  invoking `(k v)` aborts the pending computation to the capture site with `v`.

| Oracle pair | Program shape | Value |
| --- | --- | --- |
| `cps-oracle-callcc-capture` | `call/cc` capturing (invoke `k` aborts pending) and ignoring `k` (body value flows out) | `capture=42` / `ignore=11` |
| `cps-oracle-escape-abort` | `escape` one-shot abort vs. normal body value | `15` / `100` |
| `cps-oracle-escape-search` | `escape` as early-exit from a search loop | `first=7` / `none=-1` |

### 3. Cloneable (multi-shot) capture -- `cloneable-reset` / `cloneable-shift`

- **Gate:** `emit_cps_program_uses_cloneable_dk` (`src/compiler/emit_cps.c:1308`).
- **Runtime prelude:** `emit_cps_cloneable_bridge_prelude` (`emit_cps.c:1316`),
  reusing the DK deep-clone `dk_copy_range` + capture clone/drop glue.
- **Why it is the crux:** multi-shot resume needs the DK deep-clone; this is the
  load-bearing runtime code U3 ports.

| Oracle pair | Program shape | Value |
| --- | --- | --- |
| `cps-oracle-cloneable-basic` | `cloneable-shift` (receiver ignores `k`) and `cloneable-reset` passthrough | `15` / `42` |
| `cps-oracle-cloneable-multi-resume` | resume the **same** captured continuation twice (via clone) -- the multi-shot invariant | `10` / `20` |

### 4. Serial (marshalable) capture -- `serial-reset` / `serial-shift`

- **Gates:** `emit_cps_program_uses_serial_dk` (`src/compiler/emit_cps.c:1662`)
  and `emit_cps_program_contains_serial` (`emit_cps.c:1672`, gates prelude
  emission on *presence* so stdlib `save-cont!`/`resume-cont!` never dangle).
- **Runtime prelude:** `emit_cps_serial_runtime_prelude` (`emit_cps.c:1684`).

| Oracle pair | Program shape | Value |
| --- | --- | --- |
| `cps-oracle-serial-passthrough` | `serial-reset` with no shift (evaluate + return body) | `result: 42` |
| `cps-oracle-serial-roundtrip` | `Serializable` serialize/deserialize identity over int/bool | `int ok: 0/42/-1`, `bool ok: 1/0` |

### 5. Async / await -- `async` / `await`

- **Gate:** no dedicated gate -- async/await *rides the cloneable/serial
  machinery*. Surface forms elaborate to `EX_ASYNC` / `EX_AWAIT`
  (`src/compiler/elab_concurrent.c:107,145`) and lower via the scheduler in
  `src/compiler/emit_expr.c:5785` (`EX_ASYNC`) / `:5866` (`EX_AWAIT`).

| Oracle pair | Program shape | Value |
| --- | --- | --- |
| `cps-oracle-async-basic` | `(async fn)` -> future, `(await fut)` -> int result | `42` |

### 6. Interaction with `handle` / `perform`

The plan calls out the interactions between the delimited-control family and
effect `handle`/`perform` explicitly, because the unified classifier must keep
performer and handler co-located while a delimited form sits in the same
function.

| Oracle pair | Program shape | Value |
| --- | --- | --- |
| `cps-oracle-shift-under-handle` | `perform`/`handle` with a composite (`while`/`set!`) delegated continuation reading an enclosing local | `40` |

## Routing this net guards

The classification/routing U5-U7 must unify lives at:

- prelude gating + DK-machine emission: `src/compiler/emit_module.c:~6708-6738`
  (`emit_cps_program_uses_delimited` || `..._uses_cloneable_dk` ||
  `..._contains_serial` || `emit_cps_ir_program_has_emittable`).
- `emit_cps.c`'s form-presence gates (`emit_cps_program_uses_*`) -- the second
  classifier that U5 folds into the CT-IR taint fixpoint (`ensure_S`) and U7
  deletes.

## How the net is used in later phases

Per the plan's phasing, each of U1-U5 flips its family's fallback only after
its oracle twin passes under the CT-IR emit:

- **U1** (raw reset/shift/shift0): `cps-oracle-reset-shift`, `-shift0`,
  `-shift-capture-body` must stay equal after the flip.
- **U2** (call/cc + escape): `cps-oracle-callcc-capture`, `-escape-abort`,
  `-escape-search`.
- **U3** (cloneable): `cps-oracle-cloneable-basic`, `-cloneable-multi-resume`
  (the highest-risk flip -- multi-shot capture correctness).
- **U4** (serial): `cps-oracle-serial-passthrough`, `-serial-roundtrip`.
- **U5** (async): `cps-oracle-async-basic`.
- Throughout: `cps-oracle-shift-under-handle` guards performer/handler
  co-location.

A family whose port is not yet ready keeps falling back to `emit_cps.c`, so its
`-cps` twin stays green on the old emit -- the tree remains shippable mid-port.
