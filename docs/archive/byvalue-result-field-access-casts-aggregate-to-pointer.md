# By-value `Result` field access emits a carrier-pointer cast (cc error)

**Severity:** medium (blocks `.field` access on a by-value `Result` local;
forces a workaround through carrier consumers or inline-C).

## Summary

Accessing a field of a by-value `Result` local via `(.ok-val r)` (or any
`.field`) lowers as if `r` were the boxed `int64_t` carrier -- it casts the
aggregate value to a pointer (`(Result *)(intptr_t)(r)`), which is a hard C
error (`aggregate value used where an integer was expected`). The same access
on a by-value `Option` local (`(.value o)`) lowers correctly to `(o).value`,
so the defect is specific to the `Result` field-access path.

## Minimal repro

```turmeric
(defn parse [b : int] : Result
  (if (= b 0) (err "zero") (ok (* b 2))))

(defn main [] : int
  (let [r (parse 7)]
    (.ok-val r)))          ;; cc error
```

```
$ ./build/tur build /tmp/bugrepro.tur -o /tmp/bugrepro
.../_tmp_bugrepro_tur.c:5616:13: error: aggregate value used where an integer was expected
```

## Root cause

In the generated C, `r` is a by-value aggregate:

```c
Result r_1245 = parse(INT64_C(7));
__t51 = ((Result *)(intptr_t)(r_1245))->ok_val;   /* <- aggregate cast to ptr */
```

The `Result` field-access lowering assumes the carrier (boxed `int64_t`
pointer) representation and emits `(Result *)(intptr_t)(<expr>)->field`. For a
by-value `Result` the access should be `(r_1245).ok_val` (mirroring how the
by-value `Option` access already emits `(o).value`). The carrier-vs-by-value
discrimination that the Option accessor performs is missing on the Result
accessor path.

Likely in the `.field` emit path in `src/compiler/emit_expr.c` (struct/ADT
field projection), where the receiver's by-value-ness is consulted for Option
but a Result receiver still takes the pointer-cast branch.

## Workarounds (in use today)

- Feed the `Result` to a carrier consumer (e.g. `ok?`, `unwrap`) so it is held
  in the boxed `int64_t` carrier form, where the pointer-cast access is
  correct.
- Read the field in an inline-C body that receives the `Result` by value
  (`r.ok_val`), which compiles fine -- this is what the Phase 5 debugger
  fixture (`tests/fixtures/debugger-phase5/`) does to keep a `Result` live.

## Found during

debugger Phase 5 (rich type display in native debuggers) -- building a fixture
that holds a by-value `Result` local for the gdb pretty-printers. See
docs/upcoming/debugger-native-types-plan.md.

## Resolution

Fixed in `src/compiler/emit_expr.c` (the `EX_GET_FIELD` lowering). The
`through_carrier` decision keyed only on the receiver type
(`TY_STRUCT && def->n_type_params > 0`), which is true for any bare
`Result`/`Option` receiver -- including a by-value aggregate local. A
by-value carrier-ABI binding is already tracked by its
`emit_byvalue_carrier_abi` flag (set true when the binding/param is declared
as the concrete struct rather than the int64 carrier). The fix flips
`through_carrier` off when the receiver is such a by-value `EX_VAR`, so the
access takes the direct `(r).field` branch instead of the
`((Result *)(intptr_t)(r))->field` pointer cast.

This also corrects the analogous bare-`Option`-by-value case (the original
report only observed Option working because its repro happened to use a
concrete `Option__int` receiver, where `n_type_params == 0` already routed to
the direct path).

Regression coverage: `tests/fixtures/byval-result-field-access/`.
