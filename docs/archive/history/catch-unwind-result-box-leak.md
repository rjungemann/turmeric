# Fix paper trail -- catch-unwind result-box / payload leak

Resolved 2026-07-07. Report: `docs/archive/history/catch-unwind-result-box-leak-report.md`.

## Change

1. `src/compiler/emit_module.c` -- emit a new runtime helper in the panic
   preamble, right after the `tur_catch_unwind_box` / `tur_catch_panic_of_box`
   definitions (so it sees `tur_result_box_t` and the payload struct):

   ```c
   static void tur_result_box_free(int64_t __r) __attribute__((unused));
   static void tur_result_box_free(int64_t __r) {
       tur_result_box_t *__b = (tur_result_box_t *)(intptr_t)__r;
       if (!__b) return;
       if (!__b->is_ok) free((tur_panic_payload *)(intptr_t)__b->err_val);
       free(__b);
   }
   ```

   Frees the box, and for an err box the caught `tur_panic_payload` *record*.
   Does NOT free `payload->value`: `panic-with` may carry an inline scalar
   (`(void*)42`) or a value owned elsewhere, so freeing it is a nonheap-free /
   double-free. That is exactly why `panic_payload_free` (which does
   `free(p->value)`) is not reused -- calling it on the `(panic-with 42)` repro
   aborts with a `-Wfree-nonheap-object` free of `(void*)42`.

2. `src/compiler/emit_stmt.c` -- `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF`
   statement-position case now emits `tur_result_box_free(<result>)` after the
   catch. Only fires in statement position (value provably discarded); a caught
   Result that is used flows through `emit_value` and is left alone, so no
   premature free.

## Why the box appeared "not to leak" pre-fix

In the discard repro `__catch_result_N` was unused, so the C compiler DCE'd the
`tur_box_ok`/`tur_box_err` `malloc` entirely (a malloc whose result is unused
has no observable effect). The 16-byte block valgrind reported was therefore the
**fat-closure thunk** (`malloc(2 * sizeof(int64_t))` in `main`), not the
24-byte `tur_result_box_t`. Referencing the box in `tur_result_box_free` defeats
the DCE, so the box is now genuinely allocated and freed (net-neutral), while
the caught payload -- a real side-effecting allocation that was never DCE'd --
is the actual leak this fix removes.

## Verification (Debug build, `-fsanitize=address,undefined`)

| repro | before | after |
| --- | --- | --- |
| `(catch-unwind (fn [] : int 5))` discarded | 16 B lost | 16 B lost (thunk; box freed) |
| `(catch-unwind (fn [] (panic-with 42)))` discarded | 48 B lost (16 thunk + 32 payload) | 16 B lost (thunk) |
| 3 sequential panic-discards | 144 B lost | 48 B lost (16 * 3 thunk) |
| 1000-iter loop of caught+discarded panics | ~48 KB lost | 16 KB lost (thunk only) |

No new `Invalid free` / double-free under valgrind; used results
(`ok-val`/`err?` on a let-bound catch) still return correct values.

`bash tests/run.sh`: 1970 passed, 1 failed. The single failure
(`io-stdlib-roundtrip`, a glibc `corrupted size vs. prev_size` abort) uses no
catch-unwind, passes 5/5 when run standalone, and is compile-time-inert to this
change (only the `unused` helper is added to its preamble) -- pre-existing
flake, not a regression. The 125 snapshot fixtures were regenerated in the same
commit (each gains the `tur_result_box_free` helper; one gains the
statement-position free call). Regen honored each fixture's `flags` file so the
`panic-return-signal` / `stackless-catch-unwind` variants stay on their intended
lowering.

## Residual (open)

`docs/reported/catch-unwind-thunk-closure-leak.md` tracks the two remaining
items: the per-call fat-closure thunk leak (16 B / catch) and the let-bound
caught-Result box that goes out of scope without escaping (escape/last-use
analysis).
