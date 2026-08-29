# A struct-typed function passed to an arrow combinator returns garbage

**Severity: low** -- the arrow layer is documented as running on the erased
two-slot `int64` heap-pair carrier, and the working idiom (ascribe at the
edges) is a one-liner. But nothing rejects the struct-typed spelling, so the
failure is silent. Found while implementing `docs/archive/arrowloop-lazy-feedback.md`.

## Repro

```turmeric
(load "stdlib/arrow.tur")
(load "stdlib/tuple.tur")

(defn tur-step [t : (Tuple2 int int)] : (Tuple2 int int)
  (tuple2 (+ (tuple2-1st t) 1) (* (tuple2-1st t) 2)))

(defn main [] : int
  (let [lp (arrow-loop tur-step)]
    (println (lp 5)))          ;; want 6
  0)
```

Prints `-2305843009213693952` (was `0` before the lazy-feedback change --
either way, not `6`). Called directly, `(tur-step (tuple2 5 0))` is correct,
so the defect is entirely in the crossing.

## Root cause

*(Sharpened while fixing it. The first draft of this section blamed the return
ABI alone and said "the parameter side happens to survive"; both halves were
wrong, and the parameter is the half that made the typed shim get declined.)*

A wide (> 8 byte) by-value aggregate crosses a fat-closure boundary as an
**int64 heap-box pointer** -- the b4box convention, spelled by
`thunk_param_slot_c_name` (`src/compiler/emit_module.c:474`). A `defn` declared
over `(Tuple2 int int)`, though, reads a 16-byte struct out of two registers
and returns one in two more. The bridge between the two is the *typed fatshim*
(`ensure_typed_fatshim`), which already knows how to unbox such a parameter.

It was never emitted here. `use_typed_thunk_abi`
(`src/compiler/emit_module.c:420`) ANDs the result and the parameters, and
`thunk_type_has_concrete_c_abi` deliberately **declines a `TY_APP` monomorph at
or below the 16-byte sret threshold in RESULT position** -- that case is meant
to ride the generic forwarding shim, so slot 0 keeps the honest
`int64_t (*)(void *, int64_t)` spelling an erased consumer casts to. The
comment there is explicit that "only the RESULT is the erased-consumer hazard"
and that "PARAMETER position keeps the full admission" -- but the `&&` does not
know that. One declined result took the whole signature's bridge down with it,
parameters included, and `EX_FN_TO_FAT` fell back to `__tur_fatshim<arity>`:

```c
static int64_t __tur_fatshim1(void *__e, int64_t a0) {
    return ((int64_t (*)(int64_t))(intptr_t)((int64_t *)__e)[1])(a0);
}
```

So the callee got the box pointer in RDI and read `{e1 = pointer, e2 = junk}`,
and the caller read the returned aggregate's first eightbyte as a carrier
value. Nothing lied at a boundary anyone could check: slot 0's own spelling was
correct, which is why this was silent rather than a compile error.

## Working idiom

Take and return the erased carrier, ascribing at the edges:

```turmeric
(defn tur-step [p : int] : int
  (let [t (:: p (Tuple2 int int))]
    (:: (tuple2 (+ (tuple2-1st t) 1) (* (tuple2-1st t) 2)) :int)))
```

This is what `tests/fixtures/arrow-loop-lazy-feedback` and
`tests/fixtures/arrow-loop-delay` do, and it is now documented under
"Writing the looped arrow" in `docs/guides/arrows-guide.md`.

## Fix directions

1. Cheapest: reject it. The arrow combinators take `^fat f` with no declared
   function type, so nothing checks the callee's ABI today. Giving them a
   declared `(fn [int] #fx{} int)` parameter type would turn this into a
   type error at the call site instead of garbage at runtime.
2. Fuller: teach the fat-closure shim to bridge a struct-returning callee into
   the `int64` thunk ABI (an sret trampoline), so the struct-typed spelling
   works as written.

## Guides to update when fixed

- `docs/guides/arrows-guide.md` ("Writing the looped arrow")

## Resolution (2026-08-29)

Direction 2, scoped to exactly the broken set. `ensure_carrier_fatshim`
(`src/compiler/emit_module.c`) is the missing bridge, and `EX_FN_TO_FAT`
(`src/compiler/emit_expr.c`) reaches for it when `ensure_typed_fatshim`
declines but a parameter is a b4box slot:

```c
static int64_t __tur_fatshim_carrier_tur_adt_Tuple2__int__int_tur_adt_Tuple2__int__int(
        void *__e, int64_t a0) {
    tur_adt_Tuple2__int__int __r =
        ((tur_adt_Tuple2__int__int (*)(tur_adt_Tuple2__int__int))
             (intptr_t)((int64_t *)__e)[1])(*(tur_adt_Tuple2__int__int *)(intptr_t)a0);
    tur_adt_Tuple2__int__int *__b = (tur_adt_Tuple2__int__int *)malloc(sizeof *__b);
    *__b = __r;
    return (int64_t)(intptr_t)__b;
}
```

Slot 0 keeps the erased `int64_t (*)(void *, int64_t...)` spelling the call
site casts to -- the property the result-position carve-out exists to protect
-- while each wide parameter is unboxed and a wide result is boxed back into
the carrier. Boxing the result is not incidental: the carrier representation of
a wide by-value aggregate *is* a heap-box pointer, which is what the parameter
side and `ensure_catch_box_shim` already do. Bridging only the parameter was
tried first and still segfaulted, because the arrow layer then read the
returned struct's first eightbyte as its pair pointer.

Deliberately **not** reused for this: `ensure_typed_fatshim` with an int64
result type. That still casts the callee to int64-returning and hands back
`e1`; it does not box.

Applicability is held to the broken set -- at least one parameter must be a
b4box slot, and the result must be either a b4box aggregate or a plain int64
carrier. A signature whose parameters are all fine and whose result is merely a
<= 16 byte monomorph keeps the generic forwarding shim it has today, where the
register-returned aggregate passes through the tail-call to a typed consumer
untouched. A `> 16` byte (pbp) parameter is bridged as `const T *`, which is
what the generic shim already did by accident, so that case is byte-identical.
Nothing that worked before changes.

- Fixture: `tests/fixtures/arrow-struct-typed-arrow` asserts the struct-typed
  and erased-carrier spellings agree under `arrow-loop`, `arrow-loop-delay`,
  and `arrow-loop-fix`, plus the wide-param/narrow-result shape.
- Guide: `docs/guides/arrows-guide.md`, "Writing the looped arrow" -- the
  paragraph saying the struct-typed spelling does not work is replaced by the
  worked example.
- Verification: `bash tests/run.sh` 2729 passed, 0 failed; `tests/run-jit.sh`
  (c2mir, the engine the `thunk_type_has_concrete_c_abi` comment names as where
  this class of UB turns into a real wrong-sret crash) 2635 passed, 0 failed,
  56 skipped; `run-sr4-seam` green, including `fat-dispatch-wide-byval-arg` and
  `fat-dispatch-parametric-monomorph-return` -- the two prior bugs in this seam.

Not addressed, and not part of this report: `EX_POLY_TO_FAT` selects
`__tur_poly_to_fat<N>` by arity the same way. A poly-carrier value is erased to
int64 by construction, so there is no known miscompile there, but the
shim-selection shape is the same one that hid this.
