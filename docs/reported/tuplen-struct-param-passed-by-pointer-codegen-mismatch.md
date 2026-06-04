# TupleN (N>=3) struct parameter passed by pointer to a by-value callee

**Summary:** A `TupleN` value (N >= 3) used as a function *parameter* and then
forwarded to a stdlib accessor emits C that passes a `const TupleN*` where the
accessor's specialization expects a `TupleN` by value -- a hard `cc` failure.
Tuple2 is unaffected.

**Severity:** Medium -- silent until used; produces a hard build error
(`incompatible type for argument 1`), not a miscompile. Blocks any
TupleN-by-parameter code for N >= 3. Discovered while landing the `[T1..Tn]`
tuple-type surface sugar (KB-029 residual gap), but **not caused by it** --
the explicit `(Tuple3 ...)` annotation reproduces identically.

## Minimal repro

```turmeric
(defn mid [t : (Tuple3 int int int)] : int
  (tuple3-2nd t))
(println (mid (tuple3 10 20 30)))
```

(The bracket-sugar spelling `[int int int]` reproduces the same failure, since
it lowers to the identical `(Tuple3 int int int)` type.)

## Observed

```
error: incompatible type for argument 1 of
  'tuple3_2nd__spec__int64_t_Tuple3__int__int__int'
  return tuple3_2nd__spec__..._Tuple3__int__int__int(t);
         ^
note: expected 'Tuple3__int__int__int'
      but argument is of type 'const Tuple3__int__int__int *'
tur: cc invocation failed (status 256)
```

The parameter `t` is materialized in C as `const Tuple3__int__int__int *`
(by-pointer), but the specialized accessor `tuple3_2nd__spec__...` is declared
to take the struct *by value*. The call site forwards the pointer without a
deref.

## Expected

Either the parameter is passed by value (matching the accessor signature), or
the call site dereferences the pointer. The two struct-passing conventions for
the same `TupleN` type must agree. The analogous Tuple2 program compiles and
runs:

```turmeric
(defn f1 [p : (Tuple2 int cstr)] : int (tuple2-1st p))   ; OK, runs
```

## Root-cause direction

The struct-passing ABI decision (by-value vs by-pointer) is inconsistent
between (a) how a `TupleN` *parameter* is lowered and (b) how a `tupleN-Nth`
*specialization* declares its parameter. Tuple2 lands on one convention for
both; Tuple3+ diverges. Likely a size/arity threshold in the struct-ABI
classifier (small structs by value, larger by pointer) that the accessor
specialization path does not consult -- so the caller picks "by pointer" for
the bigger struct while the callee specialization was emitted "by value".

Pointers to chase:
- the struct-return/struct-arg ABI classifier in codegen (grep for the
  by-pointer threshold used for aggregate parameters);
- the `tupleN-Nth` accessor specialization emitter (does it force by-value
  regardless of the classifier?);
- why Tuple2 agrees but Tuple3 does not (size boundary?).

## Validation of a fix

- `mid` repro above compiles and prints `20`.
- `tests/fixtures/tuple-345-basic` still passes (it only does *inline*
  construct+access, never parameter passing -- which is why this stayed
  hidden).
- Add a fixture that passes Tuple3..Tuple8 as parameters to a `tupleN-Nth`
  accessor and asserts the result. The existing
  `tests/fixtures/tuple-type-bracket-sugar` deliberately keeps Tuple3 to
  *inline* use for this reason; promote it to parameter passing once fixed.
