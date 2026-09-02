# An erased generic field read overruns a sub-word monomorph's return box

**RESOLVED 2026-09-02.** See the resolution section at the end.

**Severity: medium.** Silent wrong-width read on the default path; ASan
`heap-buffer-overflow`. Found by the corpus-wide leak sweep of 2026-09-02
(`benchmarks`-style ASan build of every fixture), which ran every fixture
under AddressSanitizer for the first time. Reproduces on `main` (ef33be75)
with an unmodified compiler, so it is pre-existing, not a regression of the
reclamation work.

## Summary

`tests/fixtures/van-laarhoven-lens-wide-functor-show` reads 8 bytes from a
1-byte heap region:

```
ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 8
    #0 run_hyid                        (defn run-id [A] [i : (Identity A)] : A (.wrapped i))
    #1 __inst_Functor_fmap_Identity
    #2 deep_hylens_un_undict_un1504
0x...91 is located 0 bytes after 1-byte region
allocated by thread T0 here:
    #1 __fn_1467    { tur_adt_Identity__bool *__tur_ret_p = malloc(sizeof(tur_adt_Identity__bool)); ... }
```

The lambda `(fn [x : bool] : (Identity bool) (mk-id x))` returns the BY-VALUE
monomorph `tur_adt_Identity__bool` (`struct { bool wrapped; }`, 1 byte) and,
crossing the poly-fn boundary, heap-boxes it with `malloc(sizeof(...))`.
The erased generic reader `run-id` then dereferences that box as the erased
layout `tur_adt_Identity` (`struct { int64_t wrapped; }`) and reads 8 bytes.
The program's printed output happens to be right because the low byte is
the one that matters on a little-endian host and the heap slack after a
1-byte allocation is readable in a non-sanitized build.

## Where

- The boxing site is the aggregate return spill (`__tur_ret_p`) for a
  by-value monomorph crossing into an int64 slot; it allocates
  `sizeof(<monomorph>)`.
- The reader is the erased body of a generic accessor, which is typed
  against the erased ADT layout, whose every field is an int64 word.

The two disagree exactly when the monomorph's field is narrower than a word
(`bool`, `int8`..`int32`, `float32`). An `int` or pointer payload has the
same size in both layouts and is unaffected, which is why this only surfaces
on a `bool` instantiation.

## Fix directions

1. **Box to the erased width at the spill.** When the aggregate spill boxes
   a monomorph that an erased reader can reach, allocate and lay out the
   ERASED layout (every field widened to its int64 carrier form) rather than
   `sizeof(<monomorph>)`. This is the same widening the carrier bridges
   already do for scalars; the spill would do it per field.
2. **Or make the erased reader read the monomorph layout.** Harder: the
   erased body does not know which monomorph it was handed.

Direction 1 is local to the spill shim and is the one to take. It wants a
fixture that asserts the VALUE with a sub-word payload other than `bool`
(a `float32` or `int8` `Identity`), since a `bool` read can be right by
accident.

## Related

- `docs/upcoming/sum-representation-plan.md` -- the M6 / G6(c) notes on
  sub-word carriers folding wrong at nested nodes are the same mismatch seen
  from the constructor side.

## Resolution (2026-09-02)

Neither fix direction as written. The layout rule already had the answer
for MULTI-variant parametric monomorphs: `adt_field_c_type` (types.c) widens
a sub-word INTEGER field to the int64 slot so the monomorph agrees with the
generic layout every erased reader assumes, and its comment claimed
single-variant records "have no generic-union twin". They do -- the base
typedef of a parametric record (`tur_adt_Identity { int64_t wrapped; }`) is
what every erased generic body reads through, which is exactly this report.

The widening now applies to a record monomorph too, but only for a field
whose DECLARED type is a type parameter (`wrapped : a`): that is the field
the twin spells as `int64_t`. A record field declared concretely (`:bool`)
is `bool` in both layouts and keeps its width. So `tur_adt_Identity__bool`
is `{ int64_t wrapped; }`, the box at every crossing is 8 bytes, and the
erased read is exact rather than lucky. The typed reads and writes already
convert at the slot (the store widens, the typed binder narrows), as they
did for the multi-variant case.

Pinned by `tests/fixtures/erased-reader-subword-record-monomorph`, which
asserts VALUES through the rank-2 dict-clone crossing at `bool`, a negative
`int8` (sign extension), and an `int32` with non-zero high bytes (a
byte-punned read cannot be right by accident). Verified to fail against the
reverted compiler with the same `heap-buffer-overflow`, and to pass with the
fix. The original fixture is clean under ASan. Suite 2755/0 with one
snapshot regenerated; leak-check 76/0/0; both seams green.

**Residue, closed (2026-09-02).** `float32` keeps its 4-byte width in every
monomorph (the existing policy: an implicit float-to-int64 store would
VALUE-convert), so a `(Identity float32)` read through an erased body
overread by four bytes. Two defects hid behind it, fixed together:

- **Layout.** A record monomorph now pads a type-parameter-typed `float32`
  field to the word (`emit_registered_adt_app_rec`, `int32_t __pad_<f>`), so
  the erased int64 read stays inside the aggregate on any endianness. The
  erased reader recovers the float from the slot's FIRST four bytes
  (`tur_sc_f32_from_bits` is a memcpy -- byte position, not value), which
  is exactly where the typed store put it.
- **ABI.** With the overrun gone the values were still garbage: the
  `__poly_N` wrapper that boxes a float-typed named fn (or non-capturing
  lambda) carries the float NATIVELY (`static float __poly_N(void *,
  float)`, xmm0) so it agrees with a typed `:fn` cast and the typed
  poly-to-fat shim -- but an erased typeclass-method sink (`g : (fn [a] b)`
  in `Functor`) is compiled once for every `a` and invokes it through
  `((int64_t (*)(void*, int64_t))g.fn)`: integer registers in, RAX out.
  Same defect for a capturing lambda's own `float`/`double` thunk. See
  [history/erased-fn-sink-float-wrapper-carrier-mismatch.md](history/erased-fn-sink-float-wrapper-carrier-mismatch.md).

Pinned by `tests/fixtures/erased-reader-float32-record-monomorph` (`2.5`
and `-7.25` through the dict-clone crossing; a `float` twin, a capturing
lambda, a non-capturing lambda and concrete dispatch were probed by hand
under ASan).
