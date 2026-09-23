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
compare is there (`TUR_GETTAG(r) != TUR_TB_BOUNCE` in `__tur_tb_call`), plus
the registry lookup below. Re-measured after the lookup went O(1): 458 ms
median (402-514), the same within noise -- this program registers two
bouncers, where the linear scan was already short.

## Registry lookup

The driver arms a callee only if it is a registered bouncer, so it looks the
callee up on EVERY call it makes -- a bounce into a registered function (a
hit) or a driven call into an ordinary one (a miss, which probes both the box
and the thunk keys). It was first a linear scan of every registered box and
thunk, and that cost scales with the program, not the loop.

[`gen-saffron-dyn-tail-registry.py`](gen-saffron-dyn-tail-registry.py) writes
a program with N extra registered bouncers and one 10,000,000-call loop:
`hit` bounces through a function registered LAST (the scan's worst case),
`miss` drives a tail call into a non-bouncer. Same machine and flags as above,
median of five:

| registered bouncers | hit, linear | hit, hashed | miss, linear | miss, hashed |
|---|---|---|---|---|
| 0    | 129 ms  | 131 ms | 783 ms  | 770 ms |
| 10   | 166 ms  | 144 ms | 838 ms  | 855 ms |
| 100  | 545 ms  | 139 ms | 1302 ms | 834 ms |
| 1000 | 3350 ms | 141 ms | 4155 ms | 859 ms |

The scan cost ~0.3 ns per registered bouncer per call -- 4x at 100 bouncers
and 26x at 1000 on the hit loop. The registry is now an open-addressing hash
(load at most 1/2, linear probing, filled only from static init and
read-only after), so both columns are flat: ~1 ns per call over an empty
registry for a hit, ~8 ns for a miss. `__tur_tb_target` also returns at once
when nothing is registered, which is every Saffron program without a bouncer.

Depth is no longer the variable: the same loop at 10,000,000 deep at `-O0`
runs in ~1.4 MB peak RSS (`tests/fixtures/tailcall-dyn-deep`).

Regenerate: build `benchmarks/bench-saffron-dyn-tail.tur` with each compiler
and time the binaries; there is no runner script, since the comparison needs a
second, older `tur`.
