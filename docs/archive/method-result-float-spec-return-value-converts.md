# A float typeclass-method result crossing into a generic call is value-converted, not reinterpreted

**Severity:** silent wrong answer. No crash, no diagnostic -- `7.1` becomes
`3.45846e-323`.
**Status: FIXED 2026-08-16** (same day the first attempt was reverted; the
revert's record below is what made the second attempt land).  The producer's
bit-cast is keyed on the method's **DECLARED result kind**
(`e->type.as.fn.result_kind`, the instance-resolved signature) in the
direct-return chain in `emit_fns.c`, routed through `emit_carrier_bridge`.
That key is what the first attempt lacked, and it is what pairs producer with
consumer: the consumer bit-reinterprets exactly when the call's resolved
result type is a float -- the same declared type read from the other end.  A
method DECLARED `: int` whose body produces a float (the BoxMap
counterexample that broke attempt one) keeps its value conversion, because
for it the conversion IS the declared semantics; all four fixtures the first
attempt broke pass unchanged.

The "two consumer conventions" the revert blamed turn out not to be arbitrary
conventions at all -- they follow the declared result type, which the first
attempt could not see because it keyed on the BODY type (fires on both) after
measuring `fd->return_type` unset and `spec->result_type` already the
carrier.  The discriminator was found by instrumenting the direct-return
chain: the buggy clone reads `carrier=1 declared=FLOAT`, the counterexample
`carrier=1 declared=INT`.

Fixed at float, float64 and float32 (the composition
`(idf (m32 (:: 7.1 float32)))` that still printed bits-of-7 after the
generic-result fix now prints 7.1).  Pinned by
`tests/fixtures/method-result-float-into-generic` (both widths, both
directions, chained), engine-agreeing under turi; the int-declared control
stays pinned by `poly-to-fat-float-roundtrip`, and the pre-existing
compiled/turi divergence on THAT shape -- surfaced while writing the fixture
-- is filed as
[`int-declared-method-float-body-engine-divergence`](../reported/int-declared-method-float-body-engine-divergence.md).
Suite 2601/0; fuzzer clean including the originally-finding seed 9201.

Originally filed as OPEN. Pre-existing (reproduced on the compiler before the
2026-08-16 argcast routing change, byte-identical emitted C both sides).
**Found by:** `tests/type-fuzz-src.py --n 250 --seed 9201`, case 98.

## Minimal repro

```turmeric
(defclass FzT10 [a] (l1p1 [self : a] : a))
(definstance FzT10 [float] (l1p1 [self : float] : float self))
(defn l1g2 [A] [x : A] : A x)
(defn main [] : int
  (println (l1g2 (l1p1 7.1)))   ; expect 7.1, prints 3.45846e-323
  0)
```

Neither half fails alone -- it takes the composition:

| expression | result |
| --- | --- |
| `(= (l1p1 7.1) 7.1)` -- method alone | `true` |
| `(= (l1g2 7.1) 7.1)` -- generic identity alone | `true` |
| `(= (l1g2 (l1g2 7.1)) 7.1)` -- two generic identities, no method | `true` |
| **`(= (l1g2 (l1p1 7.1)) 7.1)`** -- one generic identity over a method result | **`false`** |

So the defect is specifically **a float typeclass-method result crossing into
a generic (carrier) call argument**.

## Root cause: producer converts by VALUE, consumer reinterprets by BITS

The emitted C makes it plain.  The specialization clone of the instance
method is declared to return the int64 carrier while its body returns the
`double` parameter:

```c
static int64_t __inst_FzT10_l1p1_float__spec__int64_t_double(double self) {
        return self;          /* C converts 7.1 -> 7 here */
}
```

`return self;` from a `double` into an `int64_t` return type is an implicit
**floating-to-integer conversion** in C, not a reinterpret.  The value is
already destroyed before it leaves the callee.

The caller then does the opposite -- a correct **bit** reinterpret:

```c
int64_t __ps_159 = (__inst_FzT10_l1p1_float__spec__int64_t_double(7.1));
double  __ps_160 = (l1g2__spec__double_double(
                      ((union { int64_t s; double d; }){.s = (__ps_159)}).d));
```

So the round trip is `7.1` --value-> `7` --bits-> `3.45846e-323`.  The
un-specialized instance is fine (`static double __inst_FzT10_l1p1_float(double
self)`); only the carrier-returning `__spec__int64_t_double` clone is wrong.

This is the campaign's canonical shape -- two sites disagreeing about a
carrier convention -- with the twist that the disagreement is
**value-vs-bits** rather than boxed-vs-unboxed, so it produces a wrong number
instead of a compile error or a crash.

## Fix direction -- ATTEMPTED 2026-08-16, REVERTED, and the revert is the finding

The obvious fix is to emit the mirror bit-cast on the producing side whenever
a carrier-returning function's body tail is float-typed:

```c
return ((union { double s; int64_t d; }){.s = (self)}).d;   /* via emit_carrier_bridge */
```

**It is wrong, and measuring it is what shows why: the CONSUMER convention is
not uniform.**  Implemented in the direct-return chain in `emit_fns.c` (the
arm that already routes several carrier cases through `emit_carrier_bridge`)
and run against the suite: **2595 passed, 4 failed** --
`poly-to-fat-float-roundtrip`, `poly-to-fat-float-named-fn`,
`poly-to-fat-bare-fat-sink` (stdout) and `map-multiword-struct-value`
(codegen).

The emitted-C diff for the first is a single line, and it is decisive:

```c
static int64_t __inst_BoxMap_boxmap_BoxW(int64_t container, tur_poly_fn_t fn) {
        double __ps_46 = (call_hyff(..., 3.5));
-       return __ps_46;                                            /* converts 7.0 -> 7 */
+       return ((union { double s; int64_t d; }){.s = (__ps_46)}).d;  /* bits */
}
```

That function has the *same shape* as the buggy one -- int64 carrier return
over a float body -- but **its** consumer reads the carrier as an integer
VALUE (the fixture expects `7`), while the consumer in this report's repro
bit-reinterprets it.  So the two consumers disagree, and no producer-side
rule can satisfy both.

**This is increment 2's `bind` cell shape exactly**, and its resolution is the
template: *"Fixing it means deciding the pairing where the entry point is
selected"* -- there, `ctx->poly_wrap_callee_carrier` is set at the call-arg
emission (where the callee is known) and consulted at the spill gate.  A real
fix here needs the same: the producer's float convention paired with the
consumer the dispatch actually selects, not chosen unilaterally.

Two things the attempt did settle, so the next attempt need not re-derive
them:

- **Where the emission is.** Not `emit_tail` (never reached for these clones)
  but the direct-return chain in `emit_fns.c` around the
  `ret_is_int64_carrier` arms.
- **Three keys, measured.** `spec->result_type` is already the int64 carrier
  for exactly this clone, so a guard keyed on it is inert. `fd->return_type`
  is unset (kind 0) on an instance-method FnDef, so that is inert too. Keying
  on the BODY's type does fire -- and is unambiguous, because the checker
  rejects the only colliding shape (`(defn f [] : int 7.1)` is **TUR-E0707**,
  whose message already makes this argument: *"a float and a non-float live in
  different register classes ... not a tolerable carrier bridge"*) -- but it
  fires on both consumer conventions, which is the over-reach above.

## Sibling found while probing: float32 is broken on a SIMPLER shape

The check this report asked for turned one up, and it is not the same cell:

```turmeric
(defn idf [A] [x : A] : A x)
(println (:: 7.1 float32))          ; 7.1        -- literal alone, fine
(println (idf (:: 7.1 float32)))    ; 1088631603 -- generic identity ALONE
```

`1088631603` is `0x40E33333`, the IEEE-754 single-precision bits of 7.1.  No
typeclass method is involved: a `float32` crossing a plain generic (carrier)
boundary is already wrong, where the `float64` twin is fine.  That is a
smaller and probably easier cell than this one, and it is unaffected by the
attempted fix above (identical output before and after).  Worth its own
report when someone picks it up.

## Why the fixtures never caught it

Two one-sided coverage gaps compounding, which is the pattern
`docs/archive/representation-consolidation-meta-plan.md` warns about:

- the method-result carrier crossing is well covered at `int`, where value
  conversion and bit reinterpretation are the same operation, so the bug is
  invisible;
- the float carrier round trip is covered for plain generic calls, which do
  not go through a carrier-returning instance clone.

Only the composition of the two exposes it, and it needs a literal with a
non-zero fractional part -- CLAUDE.md's float-probe rule exists for exactly
this class, and `7.0` would have printed `7` and looked fine.

## Guide upkeep

`docs/guides/value-representations-guide.md` -- this is an open cell:
**typeclass method result (float) -> generic carrier call argument**.  Add it
to the open-cells table when filing work on it.
