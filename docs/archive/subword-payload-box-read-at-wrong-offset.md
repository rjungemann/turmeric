# A boxed `(Option float32)` read through the carrier answered 0

**Severity: high.** A silent wrong answer, in both engines:

```turmeric
(defclass C [a] (cm [x : a] : (Option a)))
(definstance C [float32] (cm [x : float32] : (Option float32) (some x)))
(defn ri [a] [(C a)] [x : a] : a (match (cm x) (Some q) q (None) x))
(defn ru [l (forall [a] [(C a)] (-> a a)) v : float32] : float32 (l v))
(ru ri (:: 3.25 float32))      ; 0
```

Found 2026-10-01 by the type fuzzer's rank-2 class crossing, behind a
generator reject that hid it (below). **RESOLVED 2026-10-01.**

## Mechanism

The dict slot hands back the instance's `tur_adt_Option__float32` boxed
(`class-var-applied-result-untyped-in-constrained-generic.md`), and the dict
clone reads the box through the carrier layout `tur_adt_Option`:

```c
struct tur_adt_Option          { int tag; union { struct { int64_t _0; } Some; } as; };
struct tur_adt_Option__float32 { int tag; union { struct { float _0; int32_t __pad_0; } Some; } as; };
```

The float32 word pad (`erased-generic-field-read-overruns-subword-monomorph-box`)
makes the payload's size match the carrier's slot, but not its offset. After
`int tag`, a union whose widest member is 4-aligned starts at offset 4. The
carrier's union starts at 8 because it holds an `int64_t`, so the carrier read
landed on the pad. The record (named) layout has no leading tag and was never
affected. Other sub-word payloads (bool, int8/16/32, uint8) are widened to the
`int64_t` slot and were fine.

## Also: the reject that hid it

A class whose instances are all floats or aggregates (`float` and `float32`)
had no carrier-shaped representative for an abstract receiver. The dispatch
fell through to the generic search, which refused the call as ambiguous
(`TUR-E0020`) inside every constrained generic. An abstract receiver's
instance comes from the dictionary or per-spec re-resolution, so it is never
ambiguous. The search's first candidate is now kept as the representative.

## Fix

- `types.c`: a tagged-union monomorph with a padded float32 type-parameter
  field gets an `int64_t __tur_align` union member, which aligns the union like
  the carrier's.
- `elab_typeclasses.c`: on an abstract type-variable receiver, the ambiguity
  path keeps the first candidate instead of erroring.

## Verified

- `tests/fixtures/subword-payload-box-read-at-wrong-offset` (float32 /
  int16 / bool / float through rank-2 `Option`, float32 through `Result`),
  compiled and `--interpret`.
- A probe of bool, int8, int16, int32, uint8 and float32 through rank-2
  `Option` and `Result` is correct in both engines.
- The new matrix row `optf32` (214 cells) is clean.
