# Typeclass method rejects bare nominal return types (RESOLVED)

**Severity:** P1 -- blocked both remaining U2 typeclass-collapse targets
(http/httpd `Handler`, plot `Renderer`).

## Symptom

A `defclass` method whose declared return type was a **bare user-defined
nominal type** (`defopaque` or `defstruct`) failed with `unsupported return
type in typeclass method` -- even though (a) the same type was accepted as a
**parameter**, and (b) the same type **wrapped in an applied form**
(`(Result T cstr)`, `(Vec T)`) was accepted as a return type.

## Minimal repro

```turmeric
(defopaque Resp :int)
(defstruct BBox [lo : int hi : int])

(defclass MkResp [a] (mk  [x] : Resp))   ; was rejected
(defclass MkBox  [a] (mkb [x] : BBox))   ; was rejected

(defclass P [a] (p [x : Resp] : int))            ; Resp as a parameter -- OK
(defclass W [a] (w [x] : (Result Resp cstr)))    ; applied form -- OK
(defclass V [a] (v [x] : (Vec Resp)))            ; applied form -- OK
```

## Root cause

`src/compiler/elab_typeclasses.c`, return-type parsing. A single-symbol
`: T` return is normalized to `F_KEYWORD`. The `F_KEYWORD` branch matched a
fixed keyword set (`int`/`bool`/`cstr`/`void`/`ptr<void>`), then class
type-params (`class_type_param_match`), then associated-type names; if none
matched it emitted `diag_emit(DIAG_ERROR, ..., "unsupported return type in
typeclass method")`. It never attempted to resolve the symbol as an ordinary
nominal type. The adjacent `F_TYPE_ANN` / `F_LIST` / `F_VEC` branches *do*
resolve compound types via `type_expr_from_form` -- which is why
`(Result Resp cstr)` worked.

## Fix

In the `F_KEYWORD` else-branch, before emitting the error, synthesize an
`F_SYM` form from the normalized keyword and route it through
`type_expr_from_form` (the same resolver the applied-form path uses). A bare
`defopaque`/`defstruct` return now resolves; only a genuinely unresolvable
symbol still errors.

## Verification

- `tur check` / `emit-c` / `run` on the repro pass; an instance whose method
  returns a `defopaque` (`(:: x :Resp)`) and one returning a `defstruct`
  (`make-struct BBox x x`) elaborate and run end-to-end.
- Regression fixture: `tests/fixtures/typeclass-nominal-return-type/`.
- Full suite: `1689 passed, 0 failed`.
