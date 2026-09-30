# A lambda returning a function handed back a bare code pointer where a fat handle was read

**Severity: high.** Segfault, in ordinary higher-order code:

```turmeric
(defn call0i [f : (fn [] (fn [int] int))] : int ((f) 41))
(call0i (fn [] (fn [n : int] : int (+ n 1))))        ; SIGSEGV
```

The generic twin crashes the same way (`(defn call0 [A] [f : (fn [] A)] : A (f))`
at `A := (fn [int] int)`). Found 2026-09-30 by `tests/generic-spec-matrix.py`,
through its `thunk` producer at the function type (14 cells, all segfaults, the
same on the baseline compiler). **RESOLVED 2026-09-30.**

## Mechanism

fn-value-fat-normalization stage 2 settled the representation of a concrete,
effect-free function value that crosses a function boundary: it is the FAT
`{ thunk, env }` handle. Two halves carry the rule:

- the consumer side marks such a result `boxed` inside every fn-typed parameter
  annotation, so `(f)` above is read as a fat handle and `((f) 41)` fat-calls it;
- the producer side, in `elab_defn`, normalizes every tail leaf of a defn
  whose declared result is such a type onto the fat handle, and marks the
  result `boxed`.

`elab_fn` (lambdas) had only the capturing-closure half of the producer side:
a lambda whose body yields a CAPTURING closure marked its result boxed. A
captureless inner lambda stayed "thin value, thin type". Boxed into a fat
closure by `EX_FN_TO_FAT`, that lambda went behind the generic
`__tur_fatshim0`, which returns what the function returns: the bare
`tur_fnptr_int64_t_int64_t_t`. The consumer then dereferenced the code pointer
as a closure box.

A defn producer (`(defn mk [] : (fn [int] int) ...)`) was always right, which is
why the gap survived: every fixture that returned a function from a function
did so from a defn.

## Fix

`elab_fn` now applies stage 2 to a lambda whose result is a concrete effect-free
fn type, under the defn path's guards (not a nested fn result, not
`^fat`-resulted, `fn_result_type_is_fat_normalized`):
`elab_normalize_fn_tail_leaves` shims the thin tail leaves, and the result is
marked `boxed`. The lambda then returns what its consumers read, and a direct
caller of a let-bound lambda (`((g) 21)`) sees a boxed result type and
fat-calls it too. `returns_boxed_closure` follows the marked type.

## Verified

- `tests/fixtures/lambda-thin-fn-result-read-as-fat`: the concrete, capturing,
  generic, float-signature, defn, and direct-call shapes, compiled and
  `--interpret`. It segfaults on the baseline compiler.
- The matrix's `thunk/*/fn` cells pass.
