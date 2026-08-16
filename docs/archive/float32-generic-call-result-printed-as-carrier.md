# A `float32` returned from a generic call is consumed as the raw int64 carrier

**Severity:** silent wrong answer. `7.1` prints as `1088631603`.
**Status: FIXED 2026-08-16.**  The elaboration hole is closed by a paired
change: `call_wrap_reinterpret_owning` (elab_call.c) admits the
carrier<->float32 pair -- the one mixed-size float pair that IS
bit-meaningful, whose silent bail was the root cause -- and the
EX_REINTERPRET size-mismatch arm (emit_expr.c) uses the union overlay for
float pairs instead of the C value cast that would convert the bit pattern.
`(idf (:: 7.1 float32))` now prints 7.1, the spurious TUR-E0707 on an
ascribed result is gone, and compiled/turi agree.  Pinned by
`tests/fixtures/float32-generic-call-result` (both widths, argument
direction, chained calls, ascription).  Suite 2599/0 -> 2600/0 with the
fixture; fuzzer seeds 9301/9302 clean.

Residual, deliberately NOT covered by this fix, each with its own report:
a typeclass-method result feeding a generic call still fails at the PRODUCER
(`return self;` value-converts -- [`method-result-float-spec-return-value-converts`](../reported/method-result-float-spec-return-value-converts.md),
now reproducible at float32 too), and a float32-ascribed LITERAL in
comparison position emits as the double literal
([`float32-ascribed-literal-compares-as-double`](../reported/float32-ascribed-literal-compares-as-double.md),
pre-existing, engine-divergent, found while pinning this fixture).

Originally filed as OPEN with the text below.
**Found by:** probing the `float32` sibling of
[`method-result-float-spec-return-value-converts`](method-result-float-spec-return-value-converts.md),
which asked for exactly this check. Same disease, **opposite** asymmetry --
they are two different cells, not one.

## Repro -- no typeclass needed

```turmeric
(defn idf [A] [x : A] : A x)
(defn main [] : int
  (println (:: 7.1 float32))          ; 7.1         -- literal alone, correct
  (println (idf (:: 7.1 float32)))    ; 1088631603  -- WRONG
  0)
```

`1088631603` is `0x40E33333`, the IEEE-754 single-precision bit pattern of
`7.1`.

The `float64` twin is fine -- `(= (idf 7.1) 7.1)` is `true` -- so this is
specific to `float32`, and it needs only a generic identity function. That
makes it strictly smaller than the method-result cell it was found next to.

## Root cause: the ELABORATOR types the call as `int`

**Corrected 2026-08-16 after investigation.** The first reading of this report
blamed the consumer for not bridging. That was wrong, and the correction
matters because acting on it would have papered over a wrong type with a
bit-cast.

Ascribe the call and the checker says so outright:

```turmeric
(defn idf [A] [x : A] : A x)
(defn g [] : float32 (idf (:: 7.1 float32)))
```

> error [TUR-E0707]: function 'g' declares return type 'float32' but its body
> returns **int** -- a float and a non-float live in different register classes

So `A := float32` is instantiated for the ARGUMENT and lost for the RESULT:
the call's elaborated type is the erased int carrier. Confirmed at the spec
layer, where the two widths diverge:

```
[specret] idf resultkind=4  arg0kind=4   clone=idf__spec__double_double   <- float64, correct
[specret] idf resultkind=3  arg0kind=31  clone=idf__spec__int64_t_float   <- float32: result is TY_INT (3)
```

Kind 4 is `TY_FLOAT`, 31 is `TY_FLOAT32`, 3 is `TY_INT`. The float32 argument
resolves fine; only the result falls back to the carrier.

Everything downstream is a *correct consequence* of that wrong type -- which
is why the emitted C looks locally reasonable:

```c
static int64_t idf__spec__int64_t_float(float x) {
        return ((union { float s; int64_t d; }){.s = (x)}).d;   /* right, given an int64 return */
}
int64_t __ps_159 = (idf__spec__int64_t_float(7.1));
printf("%lld\n", (long long)(__ps_159));                        /* right, given an int result */
```

The producer bit-casts correctly *for the signature it was given*, and the
consumer prints an int *because the call's type is int*. Neither is the
defect.

### Fault bounded

| shape | result |
| --- | --- |
| `(:: 7.1 float32)` ascribed to a `: float32` defn | correct |
| a monomorphic `(defn mono [x : float32] : float32 x)` | correct |
| `(idf 7.1)` -- the float64 twin through the same generic | correct |
| **`(idf (:: 7.1 float32))`** | **types as `int`** |

So the fault is specifically the **generic call's result instantiation at
float32**; the ascription, the monomorphic path, and the float64 generic are
all fine.

### Note on the silence

TUR-E0707 *does* catch this the moment the result is ascribed -- the guard
exists and works. It is silent only when the value flows somewhere that
accepts an int, such as `println`, which is how the fuzzer's probe reached it.

## Superseded first reading (kept for the record)

```c
static int64_t idf__spec__int64_t_float(float x) {
        return ((union { float s; int64_t d; }){.s = (x)}).d;   /* correct: bits */
}
...
int64_t __ps_159 = (idf__spec__int64_t_float(7.1));
printf("%lld\n", (long long)(__ps_159));                        /* prints the CARRIER */
```

The producing side is right: the spec clone bit-reinterprets the `float` into
the int64 carrier. The consuming side never reinterprets it back -- it binds
the carrier as `int64_t` and hands it straight to the integer `printf` arm.
The call's result type was resolved to the erased carrier rather than to
`float32`, so no carrier->concrete bridge was inserted at the consumer.

Contrast the `float64` path, which specializes to a **concrete** return
(`..._spec__double_double`) and so needs no unbox at all. The width is what
selects the different spec shape.

## Relationship to the sibling report

They look like one family and are not, which is worth stating because the
first reading of this report assumed they were:

| | layer | defect |
| --- | --- | --- |
| [`method-result-float-...`](method-result-float-spec-return-value-converts.md) (float64) | **emit** | the type is right; the producer converts by value where the consumer reinterprets bits, and the two consumer conventions in the tree disagree |
| this report (float32) | **elaboration** | the TYPE is wrong -- the call is an `int` -- and every emitted line is a correct consequence of it |

So they need different fixes at different layers. The float64 cell wants the
producer/consumer convention paired at the point the entry point is selected
(increment 2's `bind`-cell template). This one wants the result instantiated
correctly, after which no bridging question arises at all.

## Fix direction

**In the elaborator, not the emitter.** Make the generic call's result
instantiate `A := float32` the way the argument already does, so the spec
resolves to a concrete `float` return (`idf__spec__float_float`) exactly as
the float64 twin resolves to `__spec__double_double`. Then no bridge is
needed anywhere and both widths take the same path.

Do NOT "fix the consumer" by inserting a carrier->concrete bridge at the call
site, which was this report's first suggestion: the call's TYPE is wrong, and
bridging would convert a wrong type into a right-looking value while leaving
`tur check` still believing the expression is an `int`.

Not yet located: the exact instantiation site. `elab_call.c` has several
kind whitelists that admit `TY_FLOAT` without `TY_FLOAT32`
(e.g. `:1959`, `:1979`), which is the shape to look for, but neither of those
two is on this path (they are typeclass-dispatch matchers and this repro has
no typeclass). Whoever picks this up should instrument where the call's
result type is assigned from the collected type bindings.

Check `float32` **arguments** in the same sweep; only the result direction was
probed here.

## Guide upkeep

`docs/guides/value-representations-guide.md` -- open cell: **`float32`
generic (carrier) call result -> concrete consumer**. The guide's existing
note that a float crossing the carrier "needs a bit reinterpret, not a
numeric conversion" is right and insufficient: it is also possible to
reinterpret correctly and then never unwrap, which is this bug.
