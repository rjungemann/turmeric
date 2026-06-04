# TupleN (N>=3) struct parameter passed by pointer to a by-value callee

> **Status:** FIXED 2026-06-04. Root cause and fix below; regression coverage
> in `tests/fixtures/tuplen-struct-param-passing` and
> `tests/fixtures/tuple-type-bracket-sugar`.

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

## Resolution (FIXED)

Root cause: the >16-byte pass-by-pointer threshold (`elab_structs.c:949`,
`def->pass_by_ptr = (_d_total > 16)`) makes a `TupleN` (N>=3) *parameter* a
`const T*` in C, but three callee shapes take the struct *by value* regardless
of size:

- an ABI specialization (concrete-by-value clone, e.g. a `tupleN-Nth`
  accessor) -- matched via `find_matched_abi_spec`;
- an inline-C body (declares struct params by value, DS1);
- an extern-C function.

At the call site (`emit_expr.c`, EX_CALL arg loop) the pbp-param pointer was
forwarded verbatim to these by-value formals. The existing carrier bridge only
covered an `int64_t` *carrier* argument (heap-pointer handle); a pbp param is a
real stack pointer, not a carrier, so nothing deref'd it.

Fix: when the arg is a pbp param (`expr_is_pbp_param`) and the callee takes the
struct by value (matched_spec with an aggregate formal, or inline-C, or
extern-C), emit `(*(t))` to pass a by-value copy. Tuple2 (16 bytes) is by value
on both sides and is unaffected.

Two subtleties handled:
- `expr_is_pbp_param` is tested *before* `type_struct_pass_by_ptr`, because the
  latter calls `register_struct_app` as a side effect; testing it on every
  aggregate arg spuriously registered (and emitted a typedef for) non-pbp
  carrier structs like `Map[cstr int]`. A genuine pbp param's struct app is
  already registered, so gating on it first is both correct and side-effect-free.
- The deref is mutually exclusive with the existing "callee pbp + arg by-value
  -> `&temp`" wrap and the carrier-bridge case, so no path double-processes.

Validation: the `mid` repro prints `20`; Tuple3..Tuple8 by-parameter accessors,
an inline-C-callee forward, and a pbp->pbp forward all run; full suite 1431
passed / 0 failed (ASan/LSan on); `tuple-345-basic` unchanged.

Not fixed here (separate report): the polymorphic-return-type instantiation bug
(`docs/reported/polymorphic-return-type-instantiation-collapses-to-first-tyvar.md`),
which is why the regression fixtures access tuples at a single level rather than
chaining `tuple2-2nd` over a nested tuple.
