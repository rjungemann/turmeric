# Saffron dynamic tail calls -- T6 before/after

Benchmark: [`bench-saffron-dyn-tail.tur`](bench-saffron-dyn-tail.tur) -- 2,000
rounds of a 5,000-deep loop through a function value, 10,000,000 dynamic calls
in all. The depth is chosen so the pre-T6 compiler can run it at all: before
T6 each level was a nested C call through the callee's direct-entry wrapper
(`setjmp`, a `dk_prompt` allocation, `__dk_entry_depth++`), and this
three-argument shape overflowed an 8 MB stack by 20,000 deep.

Measured 2026-09-23, x86_64 Linux, 4 cores, `tur build` defaults (`-O2`), five
alternating runs of each binary, wall clock:

| build | runs (ms) | median |
|---|---|---|
| before T6 (`main` @ T5) | 1472, 1262, 1205, 1215, 1256 | **1256 ms** |
| after T6 | 473, 461, 474, 471, 471 | **471 ms** |

So the trampoline is not a cost here; it is a **2.7x saving**. A bounce records
four words in a thread-local descriptor and returns to a driver loop one frame
below; the nested call it replaces opened and tore down a whole DK entry. The
plan budgeted "one tagged compare and a branch per bounce" (T-D6); the
compare is there (`TUR_GETTAG(r) != TUR_TB_BOUNCE` in `__tur_tb_call`), plus a
linear registry lookup to arm the callee, which is short while a program has
few bouncers.

Depth is no longer the variable: the same loop at 10,000,000 deep at `-O0`
runs in ~1.4 MB peak RSS (`tests/fixtures/tailcall-dyn-deep`).

Regenerate: build `benchmarks/bench-saffron-dyn-tail.tur` with each compiler
and time the binaries; there is no runner script, since the comparison needs a
second, older `tur`.
