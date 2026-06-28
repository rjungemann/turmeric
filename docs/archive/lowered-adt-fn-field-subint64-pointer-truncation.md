# Lowered record-ADT fn-field read truncates a sub-int64 function pointer

> **RESOLVED (2026-06-28).** Fixed and verified on the default path. The
> `defstruct-as-defadt` lowering graduated to always-on 2026-06-28 (see
> `src/runtime/experiments.c`), and the field-read fix (Fix direction 1 below)
> landed with it. The repro fixture `tests/fixtures/fn-field-unboxed`
> (`(defstruct Callback :copy [op (fn [int32] int32)])`, `(.op cb 3i32)`) now
> PASSes in `bash tests/run.sh` (1870 passed, 0 failed) and prints `9\n49` with
> no segfault. The emitted C now reads the field as
> `((int32_t (*)(int32_t))(intptr_t)((int64_t)(cb).op))(...)` -- the carrier-width
> `(int64_t)` cast, NOT the truncating `(int32_t)` cast shown in the Evidence
> section. Archived per the strict resolved-report rule; verification trail in
> `docs/archive/history/`.

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 1 fixture.

## One-line summary

Under the `defstruct-as-defadt` lowering, calling a function stored in a
fn-typed field whose signature has a SUB-int64 result -- `(defstruct Callback
:copy [op (fn [int32] int32)])`, then `(.op cb 3i32)` -- reads the field's 64-bit
function-pointer value through a cast to the fn RESULT type's C name (`int32_t`),
truncating the pointer to 32 bits before it is re-widened and called.  The call
jumps to a corrupted address and the program **segfaults at runtime** (the C
compiles cleanly).

## Minimal repro

`tests/fixtures/fn-field-unboxed` under the force-lower probe
(`TUR_FORCE_LOWER=1 ./build/tur build`), or once the experiment graduates:

```turmeric
(defstruct Callback :copy
  [op (fn [int32] int32)])

(defn double-it [x : int32] : int32
  (as int32 (* (as int x) (as int x))))

(defn main [] : int
  (let [cb (make-struct Callback double-it)]
    (println (as int (.op cb 3i32)))    ;; want 9
    (println (as int (.op cb 7i32))))   ;; want 49
  0)
```

Expected `9\n49`; under lowering it segfaults.  Passes at default (the struct
path emits a typed `tur_fnptr_..._t` field, so no truncation).

## Evidence (emitted C, force-lower)

```c
typedef struct tur_adt_Callback { int64_t op; } tur_adt_Callback;  /* fn field carried as int64 */
...
tur_adt_Callback cb_1268 = ctor_Callback((int64_t)(intptr_t)(double_hyit));
printf("%lld\n", (long long)((int64_t)(
    ((int32_t (*)(int32_t))(intptr_t)( (int32_t)(cb_1268).op ))(INT32_C(3)) )));
                                        ^^^^^^^^^^^^^^^^^^^^^^
/*  the field value (a 64-bit fn pointer in `op`) is cast to int32_t FIRST,
    dropping the high 32 bits, before being re-widened to the fn pointer.   */
```

The function-pointer cast itself (`(int32_t (*)(int32_t))`) is correct; the bug
is the extra `(int32_t)` truncating cast wrapped around `(cb_1268).op`.

## Root cause (located)

Two layers compose into the truncation:

1. `type_c_name` for a bare (non-`cfnptr`, non-`boxed`) `TY_FN` returns the
   RESULT type's C name (src/compiler/types.c ~L2982:
   `return type_c_name(type_from_kind(t.as.fn.result_kind));`).  For `op : (fn
   [int32] int32)` that is `"int32_t"`.

2. The lowered flat-record ADT field read (src/compiler/emit_expr.c,
   `EX_GET_FIELD` `adt_recv_byvalue` branch ~L5883:
   `buf_printf(&hb, "(%s)(%s).%s", cty, sv, mp)`) uses `cty =
   type_c_name(e->type)` as the cast.  With `e->type` the fn field type, `cty`
   is `int32_t`, so the int64-carried pointer is truncated.

The bug only bites a SUB-int64 fn RESULT (int32/int16/int8/float32/bool): for a
word-sized result (`(fn [int] int)`) `type_c_name` returns `"int64_t"` and the
cast is harmless, which is why the broader fn-field fixtures
(`defstruct-fn-field-single-arg`, `conv-*-fn-field-*`, the now-fixed parametric
`dot-parametric-fn-field-call`) are unaffected.

## Fix directions

The field stores the fn value as the int64 carrier, so the read must cast to the
carrier (or the typed fn-pointer), never the fn result scalar:

1. In the `EX_GET_FIELD` ADT branches, when the field's resolved type is `TY_FN`,
   use `"int64_t"` (the carrier the field is stored as) as `cty` instead of
   `type_c_name(field_type)` -- the call site's `((R (*)(A...))(intptr_t)...)`
   cast then forms the real pointer.  (Narrowest; mirrors how the fn-pointer cast
   is already built from the full fn type at the call site.)  The same guard
   likely belongs on the `heap_adt_recv` / `recv_carrier_byval` arms for
   consistency, though only the by-value flat-record arm is exercised here.

2. Alternatively, give `type_c_name(TY_FN)` a carrier/fn-pointer answer for a
   bare fn reference rather than the result type's C name -- but that is used
   widely and risks broad codegen churn; prefer the localized field-read fix.

## Notes

- Default suite is unaffected (only the lowered record-ADT representation carries
  the fn field as int64).
- Confirmed pre-existing: `fn-field-unboxed` segfaults under force-lower at HEAD
  (with the probe, without any local change), so it is not a regression from the
  parametric fn-field type-param inference work
  (docs/archive/lowered-adt-ctor-skips-fn-field-type-param-inference.md), which
  names this as a distinct sub-int64 fn-field sub-root.
