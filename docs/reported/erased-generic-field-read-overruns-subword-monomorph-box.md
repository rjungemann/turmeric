# An erased generic field read overruns a sub-word monomorph's return box

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
