# `#lang r7rs`: small scratch leaks T8 left in the prelude

**Severity:** low. A few prelude paths still allocate memory for one call and
never free it. Each is small per call; the list is here so the next pass
does not have to re-derive it. Found by r7rs-lang-plan T8's audit, after its
fixes (see the T8 note in docs/upcoming/r7rs-lang-plan.md for what was fixed).

Measured over the `r7rs-*` fixtures, compiled with ASan and run with
LeakSanitizer on, 2026-09-25:

| site (allocation frame) | objects | fixtures | what it is |
|---|---|---|---|
| `r7rs_ratio_part` (bignum.tur; interpreter twin `r7rs_ratio_part`) | 142 | 5 | the numerator and denominator spellings `r7rs-ratio-of-str__` splits a ratio literal into; `r7rs-big-norm__` keeps a spelling only when the part is a bignum |
| `r7rs_ns_dup`, `r7rs_ns_complex_part` | 75 + 75 | 2 | the same for a complex literal's parts (`r7rs-complex-of-str__`) |
| `vec_new` / `vec_push_ex` under `r7rs-cps-of-cstr__` | about 50 vectors | 11 | a literal (cstr) string decoded to code points for a string operation, the vector dropped after (the other `Vec int` allocations in the audit -- `string-append`, `string-copy`, `list->string`, `bytevector` -- are new Scheme strings, i.e. data) |
| `r7rs-string-of-code__` | 40 | 8 | `#\x` char spellings in the printer and the reader's `#\name` (`r7rs-rd-char-name__`) |
| `r7rs-string-append2__` | 23 | 8 | messages built for errors that are then raised, and a few spellings |
| `r7rs-eval-c-result-text__` (and its interpreter twin) | 31 | 1 | the `eval` bridge's copy of each result's text, read back into a datum and dropped; a raised condition's text becomes its message, so free only the datum path |

Not listed: Scheme data (pairs, strings, vectors ... see
[r7rs-heap-data-never-reclaimed](r7rs-heap-data-never-reclaimed.md)), the
rest-argument chains a variadic call builds (the same model), and the records
a caught `raise` leaves
([r7rs-caught-raise-leaks-runtime-records](r7rs-caught-raise-leaks-runtime-records.md)).

## Fix directions

Each is the pattern T8 used for the others: free the scratch string with
`r7rs-cstr-free__` once its bytes are copied, or build into one `R7rsIo`
buffer, and free a scratch `Vec` with `vec-free-o`. Two cautions T8 ran into:

- `r7rs-big-norm__` keeps its argument inside an `R7rsBig`; free a part only
  when the result came back an int.
- Under a Turmeric entry file the prelude is checked with the affine rules,
  and a procedure that hands an `R7rsIo` or `R7rsIdTab` to one defined LATER
  in the file is read as giving it away (TUR-E0005). Define the helper first.
