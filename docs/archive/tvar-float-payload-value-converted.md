# A float stored in a TVar was truncated, and a CAS against the wrong value succeeded

**Severity: high.** A silent wrong answer in STM, with no diagnostic and no crash:

```turmeric
(let [t (tvar/new 7.1)]
  (atomically (stm (tvar/cas t 7.4 3.25))))     ; compiled: true   (7.4 is not 7.1)
(:: (atomically (stm (tvar/read t))) float)     ; compiled: 3.45846e-323
```

The interpreter answered `false` and `7.1`. Found 2026-09-30 by the
emitted-C float lint, running inside the type fuzzer's tvar seam.
**RESOLVED 2026-09-30.**

## Why nothing caught it

The fuzzer's seam table listed the tvar seam as a *correct positive control*.
Its oracle asserted only that a CAS of the same value round-trips. Every side
of that CAS truncated the same way, so `7.1` (stored as 7) matched `7.1`
(compared as 7), and the oracle passed. The lint is what saw the
`(void*)(intptr_t)1.25` in the emitted C.

## Mechanism

`EX_TVAR_NEW`, `_WRITE`, `_SWAP` and `_CAS` (`emit_expr.c`) put each payload
into the TVar's one `void *` slot through `(void*)(intptr_t)v`. For a float that
is a value conversion. The read-back ascription `(:: w float)` then
bit-reinterpreted the integer 7: `3.45846e-323`.

The interpreter had two siblings in the same seam:

- `(:: w cstr)` of the word: a same-size ascription elaborates to an
  `EX_REINTERPRET`, whose arm re-tagged only floats. It printed the string's
  address.
- `(:: w float32)` value-converted the double's bits with `(double)v.as_int`.

The compiled `(:: w float32)` was invalid C. The narrower ascription is not a
same-size reinterpret, so the erased `EX_ASCRIBE` put the raw `void *` into a
float context.

## Fix

- The four TVar payload sites go through `tvar_payload_word`, which spells a
  float kind as its bits (`emit_word_slot_bits`, the rule the future and
  generator slots already follow).
- A `ptr` word ascribed to `float32` is bridged carrier → concrete, reading the
  low half that the store put there.
- Interpreter: the REINTERPRET arm re-tags a word to a cstr, and the float32
  ascription reinterprets the double's bits.
- The fuzzer's tvar seam now also prints the value read back through an
  ascription to the payload type. Run against the old compiler, it reports the
  bug.

## Verified

`tests/fixtures/tvar-float-payload-value-converted` (float, int, bool, cstr,
float32; CAS, write, swap, read), compiled and `--interpret`. The baseline
compiler fails it.
