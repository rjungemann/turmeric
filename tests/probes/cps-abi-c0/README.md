# CPS-IR-to-C backend -- Phase C0 ABI de-risking sketches

Throwaway, hand-written C that proves the calling convention proposed in
[`docs/archive/cps-ir-to-c-backend-plan.md`](../../../docs/archive/cps-ir-to-c-backend-plan.md)
(section "C0 result -- ratified ABI") **before** the emitter that will produce
this C exists. Neither file is emitted by the compiler; each transcribes a
colored function into the ABI by hand, node-for-node against
`tur check --dump-cps`, and compiles against the real DK runtime
(`src/runtime/cps_prompt.c`), the same machine `emit_cps_runtime_prelude`
ports into generated programs.

| File | Models | Proves | Prints |
| --- | --- | --- | --- |
| `mixed.c` | `tests/fixtures/cps-mixed-coloring/input.tur` | all three direct<->CPS edges (`cps->cps` / `cps->direct` / `direct->cps` entry), two `letcont` join points, a local `reset`/`shift` | `41` |
| `xfn_resume.c` | a cross-function delimited-control program | a `shift` in a callee reaching a `reset` in its caller, capturing a sub-continuation spanning both frames, resumed multi-shot via `dk_invoke` | `422` |

## Build and run

```sh
cd tests/probes/cps-abi-c0
cc -std=c11 -Wall -Wextra mixed.c      ../../../src/runtime/cps_prompt.c -o /tmp/mixed      && /tmp/mixed
cc -std=c11 -Wall -Wextra xfn_resume.c ../../../src/runtime/cps_prompt.c -o /tmp/xfn_resume && /tmp/xfn_resume
```

Both are also clean under `-fsanitize=address,undefined` with LeakSanitizer
enabled:

```sh
cc -std=c11 -Wall -Wextra -fsanitize=address,undefined mixed.c \
   ../../../src/runtime/cps_prompt.c -o /tmp/mixed_san
ASAN_OPTIONS=detect_leaks=1 /tmp/mixed_san
```

They are not part of `tests/run.sh` (no `expected.*`); they are a one-shot C0
artifact kept for reproducibility.
