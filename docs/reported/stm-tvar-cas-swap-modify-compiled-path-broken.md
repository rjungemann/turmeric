# Compiled path: `tvar/cas` and `tvar/swap` fail to link; `tvar/modify` is a no-op

**Summary:** Three of the six STM TVar primitives are non-functional on the
**compiled** path (`tur build` / `tur run`):

- `tvar/cas` -- codegen emits a call to `tur_tvar_cas`, but the emitted
  standalone runtime never defines it -> **link error** (`undefined reference
  to 'tur_tvar_cas'`).
- `tvar/swap` -- same: codegen calls `tur_tvar_swap`, which is never emitted ->
  **link error**.
- `tvar/modify` -- codegen emits a **no-op stub** (`void *tmp = NULL;`) that
  ignores the TVar and the function, so the modify never happens and the
  returned old value is always `NULL`.

`tvar/new`, `tvar/read`, `tvar/write`, `atomically`, `stm`, `retry`, `check`,
and `or-else` compile and link correctly.

**Severity:** High for `cas`/`swap` (a program that uses them does not build at
all); medium for `modify` (silent miscompile -- it compiles and runs but does
nothing). Half the documented `tvar/*` API (stdlib/stm.tur documents all six)
is unusable from compiled code.

## Minimal repro

```turmeric
;; cas.tur
(defn main [] : int
  (let [tv (tvar/new 0)]
    (println (atomically (stm (tvar/cas tv 0 1)))))   ; expect: true
  0)
```

```sh
./build/tur run cas.tur
# .../cas.tur.c: warning: implicit declaration of function 'tur_tvar_cas'
# undefined reference to `tur_tvar_cas'
# collect2: error: ld returned 1 exit status
```

Same shape for `tvar/swap`. For `tvar/modify`, the program links and runs but
the TVar is never modified.

## Root cause

The codegen emits the calls but the emitted runtime omits the definitions:

- `src/compiler/emit_expr.c`:
  - `EX_TVAR_CAS` (~`:3461`) emits `tur_tvar_cas(tur_stm_current_tx(), ...)`.
  - `EX_TVAR_SWAP` (~`:3450`) emits `tur_tvar_swap(tur_stm_current_tx(), ...)`.
  - `EX_TVAR_MODIFY` (~`:3438`) emits `/* TVar::modify ... */ void *<tmp> =
    NULL;` -- a stub that calls neither the function nor `tur_tvar_write`.
- `src/compiler/emit_module.c` (~`:2394-2525`) emits the STM runtime
  (`tur_tvar_new`, `tur_tvar_read`, `tur_tvar_write`, `tur_stm_commit`,
  `tur_stm_retry`, `tur_stm_check`, `tur_stm_new_transaction`,
  `tur_stm_current_tx`/`set_current_tx`) but **not** `tur_tvar_cas`,
  `tur_tvar_swap`, or `tur_tvar_modify`.

Note: `src/runtime/stm.c` *does* contain correct `tur_tvar_cas`, `tur_tvar_swap`,
and a (partial) `tur_tvar_modify` -- but that translation unit is used by the
interpreter/`libturi`, not linked into the emitted single-file C program, which
carries its own inlined runtime from `emit_module.c`. (And the runtime
`tur_tvar_modify` at `src/runtime/stm.c:166` itself writes back the *old* value
rather than the function's result -- a separate latent bug, though it is never
reached from codegen.)

## Proposed fix directions

1. In `emit_module.c`, emit definitions for `tur_tvar_cas` and `tur_tvar_swap`
   alongside `tur_tvar_read`/`tur_tvar_write` (they are short -- read-current,
   compare/replace via the write log; see `src/runtime/stm.c:176-194` for the
   reference logic).
2. In `emit_expr.c` `EX_TVAR_MODIFY`, emit a real read-modify-write: read the
   current value, call the function on it, write the result back, return the old
   value. Emit a `tur_tvar_modify` helper (taking a fat-closure / function
   pointer) in `emit_module.c`, mirroring the other primitives.
3. While there, fix the runtime `tur_tvar_modify` in `src/runtime/stm.c:166` to
   write `fn(old)` rather than `old`.

## How to validate a fix

```sh
./build/tur run cas.tur     # should print: true (currently: link error)
```

Add a compiled fixture (`tests/fixtures/stm-cas/` with `input.tur` +
`expected.stdout`) exercising cas/swap/modify and confirm `bash tests/run.sh`
is green.

## Interpreter status (for contrast)

The tree-walking interpreter (`tur --interpret`) implements all six primitives
correctly as of Phase TI4 (`src/turi/eval.c`, `case EX_TVAR_*`), including a
correct read-modify-write `tvar/modify`. The interpreter coverage lives in the
`tur_eval_stm` ctest target (`tests/turi/eval-stm.sh`). The interpreter is
therefore currently *more complete* than the compiled path for `cas`/`swap`/
`modify`; this report tracks closing the gap on the compiled side.
