---
title: Typeclass method dispatch on an opaque/rc receiver silently miscompiles under `--interpret` (carrier-fallback instance method runs as a relay)
category: Bug Report -- interpreter / typeclass dispatch
severity: High. A positive program prints a wrong value with exit 0 (the worst failure mode per CLAUDE.md). Bounded to the `--interpret` path; the compiled path is correct. Surfaces for any `(.method obj)` where `obj`'s type dispatches through the **carrier-fallback** instance method (an opaque `defopaque` handle, an `rc<T>`, or any receiver whose instance method emits as the abstract `__inst_<Class>_<method>_T` carrier symbol rather than a concrete `__inst_..._<Type>`).
status: RESOLVED 2026-06-15. Root cause was narrower than the carrier-relay
theory below: the interpreter prelude (`wk_register_typeclass_natives`,
`src/main.c`) hard-registered `__inst_Show_show_T` -> `native_show_float` as a
legacy float hack. The `_T` suffix is the ABSTRACT/carrier mangling (not
float-specific -- TY_FLOAT now mangles to `_float`), so that registration
HIJACKED every user `Show` instance over a carrier-typed receiver and returned
"0" for any non-float arg. Fix: drop the `__inst_Show_show_T` prelude
registration (`Show [float]` is served by `__inst_Show_show_float`), letting the
user's own instance method resolve. Regression fixture:
`tests/fixtures/show-instance-over-opaque-carrier/`. This report also
reclassified `exg5-rc-in-exists` out of `turi-inline-c-silent-miscompiles.md`.
---

# Carrier-fallback instance method runs as a carrier relay under `--interpret`

## Resolution (2026-06-15)

The live root cause was simpler than the carrier-relay theory in "Root cause"
below (which traced a real EX_CLOSURE relay FnDef but missed that a prelude
native shadows it). Instrumenting `turi_env_set` showed the interpreter prelude
binds, before the user program runs:

```
__inst_Show_show_int   -> native_show_int
__inst_Show_show_float -> native_show_float
__inst_Show_show_T     -> native_show_float   <-- the culprit
__inst_Show_show_bool  -> native_show_bool
__inst_Show_show_cstr  -> native_show_cstr
```

`__inst_Show_show_T` (`src/main.c` `wk_register_typeclass_natives`) was a legacy
hack from when `TY_FLOAT` mangled to the `_T` carrier suffix. It now mangles to
`_float` (served by `__inst_Show_show_float`), so the `_T` binding only ever
catches **user** `Show` instances whose receiver is carrier-typed (opaque
handle, `rc<T>`). `native_show_float` reads `a[0]` as a float; for any
non-float receiver `a[0].tag != TURI_FLOAT`, so it formats `0.0` -> `"0"`. The
EX_FN_DEF that would register the user's own instance keeps the pre-existing
native (an inline-C-bodied method is intentionally not allowed to clobber a
registered native), so the hijack wins.

**Fix:** delete the `__inst_Show_show_T -> native_show_float` prelude
registration. `Show [float]` is unaffected (it resolves via
`__inst_Show_show_float`); user carrier-typed `Show` instances now resolve to
their own method. Outcomes:

- A **pure-Turmeric** carrier-typed `Show` instance now returns the real value
  under `--interpret` (was silently `0`). Pinned by
  `tests/fixtures/show-instance-over-opaque-carrier/` (prints `WIDGET!`).
- An **inline-C-bodied** carrier-typed `Show` instance (exg5) now produces the
  clean "inline-C not supported" carve-out error instead of a silent `0` --
  the accepted turi behaviour for inline-C fixtures.

Validation: `tests/run.sh` 1647/0; `tests/run-turi.sh` 1206 passed / 2 failed
(the 2 are pre-existing `eq-carrier-capturing-comparator` / `mutmap-eq`);
`exg5-rc-in-exists` no longer silently miscompiles.

> The original investigation notes below are kept for the record; the
> carrier-relay FnDef they describe is real but was not the load-bearing cause
> (the prelude native shadowed it before it could be called).

## Minimal repro

```turmeric
(defclass Show [a] (show [x] : cstr))
(definstance Show [rc]
  (show [x] : cstr
    ```c
    char *buf = (char *)malloc(32);
    int64_t v = *(int64_t *)((RcControlBlock *)x)->value;
    snprintf(buf, 32, "%lld", (long long)v);
    return (const char *)buf;
    ```))
(defn main [] : int
  (let [r (rc/of 99)]
    (println (.show r)))      ; expected 99
  0)
```

```sh
$ ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret repro.tur
0          # WRONG, exit 0
$ ./build/tur run repro.tur
99         # correct
```

The canonical fixture is `tests/fixtures/exg5-rc-in-exists/` (expected
`99\ndone`, interpreted `0\ndone`).

## Observed vs expected

- **Observed:** `(.show r)` returns `0` (or, with a `Show [int]` instance, the
  raw rc pointer value) -- silently, exit 0. The user's `show` inline-C body is
  **never executed** (no `TUR_IC_TRACE` line, no "inline-C not supported"
  error).
- **Expected:** parity with the compiled path -- dispatch the `Show [rc]`
  instance and produce `99` (or, since the body is inline-C, at minimum a clean
  "inline-C not supported" error -- the documented turi carve-out behaviour).

## Root cause (traced this session)

The compiled C for `(.show r)` is a **direct, statically-resolved** call to the
carrier-fallback instance method:

```c
static const char * __inst_Show_show_T(RcControlBlock * x) { ... user body ... }
...
puts(__inst_Show_show_T(r_887));     /* r_887 : RcControlBlock * */
```

Note the `_T` suffix: because `rc` (and other opaque handles) dispatch through
the **carrier ABI**, the instance method emits as the abstract
`__inst_Show_show_T` symbol, not a concrete `__inst_Show_show_<Type>`. A
concrete user-struct receiver (e.g. `Show [Point]`) instead resolves to
`__inst_Show_show_Point`, whose interpreter closure carries the real inline-C
body and so reaches it (printing the clean "inline-C not supported" carve-out
error).

Under `--interpret`, instrumentation in `src/turi/eval.c` shows:

1. The elaborated `(.show r)` is an `EX_CALL` with
   `fn_binding->name->name == "__inst_Show_show_T"` (confirmed at the
   `eval_drive` callee-resolution site, `eval.c:4424`).
2. `eval_lookup` resolves that name to a `TURI_CLOSURE` (tag 5) -- **but not the
   user's inline-C method**. `eval_apply` is never entered for it (the
   `apply ...` trace never fires), so the closure is dispatched through the
   work-stack **fold** path (`DK_CALL_RET`), which only runs **turi** bodies in
   the loop. An inline-C leaf would have gone through `eval_apply`.
3. Therefore `__inst_Show_show_T`'s interpreter closure has a **turi body** --
   the synthesized **carrier relay** (cf. the compiler's carrier-relay closure
   pass, `src/compiler/emit_module.c`, "Emit generic-of-generic carrier callees
   via carrier-relay closure"). The relay was designed to forward the int64
   carrier word on the compiled path; run directly by the tree-walker it returns
   the carrier word **verbatim** (the rc handle, which reads back as `0` here)
   without ever dispatching to the real `Show [rc]` inline-C impl.

So the dispatch silently bottoms out in the carrier relay instead of the user's
method. The earlier `turi-inline-c-silent-miscompiles.md` listing of
`exg5-rc-in-exists` is a misattribution: the inline-C evaluator's matchers are
not involved (the inline-C body is never reached).

## Why it is distinct from the snprintf-matcher hardening

A separate, correct hardening landed this session in `ic_format_snprintf_call`
(decline when a snprintf-arg expression cannot be evaluated, rather than
formatting a guessed `0`). It does **not** address this report: for exg5 the
snprintf matcher already declines (it bails on `->`, `eval.c:3462`) and the body
is never reached anyway. This report is upstream of the inline-C layer entirely.

## Proposed fix directions

1. **Resolve `(.method obj)` to the real instance method, not the carrier
   relay, under the interpreter.** When `fn_binding` names a carrier-fallback
   `__inst_<Class>_<method>_T` and the instance's concrete `method_impls[i]` is
   available (it is -- `EX_DICT` at `eval.c:6053-6056` already finds it via
   `inst->method_impls[i]`), dispatch to that impl instead of the relay. This is
   the parity-preserving fix.
2. **Make the carrier relay decline in the tree-walker.** If a carrier-relay
   turi body is asked to stand in for a method whose real impl is inline-C,
   error cleanly ("inline-C not supported") rather than forwarding the carrier
   word. Converts the silent miscompile to the accepted carve-out error.
3. **Register carrier-fallback instance methods as natives that re-dispatch.**
   Bind `__inst_<Class>_<method>_T` in the interpreter to a native that looks up
   the receiver's instance and calls its concrete `method_impls[i]`.

Direction 1 is the cleanest and matches the compiled semantics; direction 2 is
the minimal "stop the silent miscompile" change if 1 proves invasive.

## Validation

After a fix:

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/exg5-rc-in-exists/input.tur
# => 99\ndone  (direction 1)  OR a clean "inline-C not supported" error (direction 2)
```

- `bash tests/run-turi.sh` stays at its baseline (currently
  `1205 passed, 2 failed` -- the 2 are the pre-existing
  `eq-carrier-capturing-comparator` / `mutmap-eq`).
- `bash tests/run.sh` unaffected (compiled path already correct).

## Pointers

- `src/turi/eval.c:4424` -- `eval_drive` callee resolution (`fn_binding` is
  `__inst_Show_show_T`).
- `src/turi/eval.c:6032` `EX_DICT` -- already has `inst->method_impls[i]`, the
  real impl direction 1 would dispatch to.
- `src/compiler/emit_module.c` -- the carrier-relay closure pass that the
  interpreter inherits and mis-runs.
- `docs/reported/turi-inline-c-silent-miscompiles.md` -- the report that
  previously (mis)listed `exg5-rc-in-exists`.
