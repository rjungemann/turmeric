# A return-dispatched class method that mints an Option over a value struct miscompiles in a constrained instance

**Resolved 2026-09-03** -- all three, see the resolution at the end; pinned by
`tests/fixtures/return-dispatched-sum-mint-constrained-instance` (leak-checked).

**Severity: medium** -- a hard `cc` error (never a wrong answer) on the
DEFAULT path, in a shape that `tur check` accepts.  Found 2026-09-03 while
writing a fixture for the fourth round of
[value-struct-payload-sum-monomorph-box-has-no-owner](value-struct-payload-sum-monomorph-box-has-no-owner.md):
the shape needed was "a class method whose result IS the class variable
mints a fresh `(Option Box)`", and every spelling of it failed to build.  Three
spellings, three different failures.

## Repro 1 -- `(some (:: (mk n) A))` in the parametric instance

```turmeric
(defstruct Box [n : int])
(defclass EncMk [a]
  (enc [x] : int)
  (mk [n : int] : a))
(definstance EncMk [int]  (enc [x] : int x)        (mk [n] n))
(definstance EncMk [Box]  (enc [b : Box] : int (.n b)) (mk [n] (make-struct Box :n n)))
(definstance EncMk [Option]
  [(EncMk A)]
  (enc [x] : int (if (some? x) (enc (unwrap x)) -1))
  (mk [n] (some (:: (mk n) A))))
(defn enc-mk [a] [^EncMk a w : a n : int] : int
  (enc (:: (mk n) a)))
(defn main [] : int
  (println (enc-mk (some (make-struct Box :n 0)) 11))
  0)
```

```
In function '__inst_EncMk_mk_Option__spec__tur_adt_Option__Box_int64_t':
error: aggregate value used where an integer was expected
    tur_adt_Option__Box __ps_245 = (some__spec__tur_adt_Option__Box_tur_adt_Box((*(tur_adt_Box *)(intptr_t)(__ps_244))));
```

`__ps_244` is the by-value `tur_adt_Box` the re-dispatched `(mk n)` returns
under the `A = Box` spec, and the construct seam still derefs it as the int64
carrier.  This is the construct/return companion of
[nested-construct-byvalue-decode](../../tests/fixtures/nested-construct-byvalue-decode/input.tur),
which covers the same seam only THROUGH a reader (`(some (ok-val (:: (dec
tag) (Result A cstr))))`); a bare ascribed return dispatch as the ctor
argument is not covered.

## Repro 2 -- the same, minted through the reader shape that does compile

Replace the parametric instance's `mk` with the reader form:

```turmeric
(defclass Dec [a] (dec [tag : int] : (Result a cstr)))
(definstance Dec [int] (dec [tag] (ok tag)))
(definstance Dec [Box] (dec [tag] (ok (make-struct Box :n (+ tag 90)))))
(definstance EncMk [Option]
  [(EncMk A) (Dec A)]
  (enc [x] : int (if (some? x) (enc (unwrap x)) -1))
  (mk [n] (some (ok-val (:: (dec n) (Result A cstr))))))
```

```
In function '__inst_EncMk_mk_Option':
error: incompatible types when returning type 'long int' but 'tur_adt_Option__int' was expected
    if (tur_panicking) return ((int64_t)0);
    { tur_adt_Option__int *__tur_ret_p = malloc(...); *__tur_ret_p = __ps_50; return (int64_t)(intptr_t)__tur_ret_p; }
warning: initialization of 'int64_t (*)(int64_t)' from incompatible pointer type 'tur_adt_Option__int (*)(int64_t)'
    .mk = __inst_EncMk_mk_Option,
```

The CARRIER BASE clone of `mk` (the one the dictionary slot points at) is
declared with the by-value `tur_adt_Option__int` result while its body still
returns the heap-spilled carrier -- the representative-int by-value result
leaked into the base clone's signature.  The specs are fine; only the
dictionary slot's clone disagrees with itself.

## Repro 3 -- a concrete applied instance head instead

```turmeric
(definstance EncMk [(Option Box)]
  (enc [x] : int (if (some? x) (.n (unwrap x)) -1))
  (mk [n] (some (make-struct Box :n n))))
```

Three independent failures on this head: `Box` in `(Option Box)` is read as
a TYPE VARIABLE named `Box` (`expected (type-app Option Box), got (type-app
Option tyvar 'Box')` when `x` is passed to a defn taking `(Option Box)`);
`(some? x)` on the instance's own parameter reports `TUR-E0005 use-after-move`
(the same body as a plain `defn` is accepted); and `(match x (Some b) (.n b)
...)` cannot resolve `.n` on the binder.  A plain
`(defn f [x : (Option Box)] : int (if (some? x) (.n (unwrap x)) -1))` is fine,
so all three are specific to the instance-method parameter.

## What is affected

Any user code whose class method returns the bare class variable and whose
instance for `Option` (or any parametric sum) builds that value from a
re-dispatched element.  The stdlib does not do this today (its `pure`-shaped
methods are HKT and go through the M7 by-value path).  The reader-mediated
shape (repro 2's inner `dec`) compiles and runs, which is what the
value-struct-payload fixture pins instead.

## Fix directions

- Repro 1: the ctor-argument construct seam should consult the re-dispatched
  callee's actual result spelling (the same `reresolved_callee` the argument
  loop already looks up) before choosing the carrier deref.
- Repro 2: the base clone's result type must stay the carrier when the body's
  tail is the heap-spill `__tur_ret_p` form; the by-value commit belongs to
  the specs only.
- Repro 3 is a separate elaboration gap (concrete applied instance heads) and
  may be worth its own report if anyone needs the shape.

## Resolution (2026-09-03)

All three were one-line disagreements between sites that are supposed to
move in lockstep, and each fix is at the site that was wrong:

- **Repro 1** -- the carrier-producer argument disjunct (emit_expr.c, the
  by-value spec-param bridge) treats any `__inst_` callee without a matched
  spec as "emits the carrier".  A return-dispatched call re-resolved to a
  CONCRETE instance whose declared result is a by-value aggregate returns
  that aggregate; `emit_reresolved_returns_byvalue_aggregate` asks the
  re-resolved binding and the deref is skipped.  (The hoist temp is not in
  the recorded-spelling table -- only pointer spellings are recorded there
  -- so the general recorded-spelling guard could not see it.)
- **Repro 2** -- the definition header (emit_fns.c), the forward declaration
  (emit_module.c) and the panic-return type all conditioned "this base clone
  spills to the int64 dict slot" on `type_uses_carrier_abi(body type)`.  A
  body that constructs the representative monomorph BY VALUE (`(Option
  int)`) is not carrier-ABI, so the header said `tur_adt_Option__int` while
  the tail (whose fallback is the result kind, int64) spilled.  The
  conjunct is gone: with no `result_full_type`, any by-value aggregate body
  of an `__inst_` method is spilled, so its header is int64.
- **Repro 3** -- two halves.  Both instance-head argument parsers
  (`parse_instance_head_arg` and the single-argument applied-head path) used
  the value-preferring `scope_lookup`, which for a lowered defstruct returns
  the constructor function; the type namespace is consulted as the
  two-parameter path already did.  And a RESOLVED applied head took a
  blanket `CK_MOVE`, so the instance method's own parameter was move-only
  and `(some? x)` consumed it; a resolved head now takes its type's own
  discipline (copy, or the opaque's linear/affine lift via
  `propagate_app_discipline`) and only an unresolved head keeps `CK_MOVE`.

With these, the value-struct report's dynamic-receiver admission has its
in-tree program: `enc-mk`'s `(Option Box)` spec frees the re-dispatched
`mk`'s payload after `enc` returns.
