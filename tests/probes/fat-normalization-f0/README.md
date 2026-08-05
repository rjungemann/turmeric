# fat-normalization-f0 -- ABI ratification probe (increment 1, stage 0)

Hand-written C proving the calling convention proposed by
`docs/upcoming/fn-value-fat-normalization-plan.md` stage 1, before the
emitter change exists -- the `cps-abi-c0` pattern applied to the fn-value
axis. See the header of `fatparam.c` for the Models / Proves table.

The one-sentence claim being ratified: **a nominal fn-typed parameter always
holds a fat `{thunk, env}` handle and every invoke is the slot-0 protocol;
bare fns are shimmed into a `{shim, orig}` box at the boundary** -- and one
compiled callee body serves capturing closures and shimmed bare fns alike,
including through a pass-through (`thru`) parameter.

Layouts are transcribed from real `./build/tur emit-c` output (2026-07-30):
the drop-glue header before every fat allocation, `__fn` at slot 0 of a
capturing env, `{shim, orig}` for the bare box, and the
`(*(thunk**)h)(h, ...)` dispatch.

Build + run (ASan/UBSan clean is part of the ratification):

    cc -std=c11 -Wall -Wextra -fsanitize=address,undefined \
       tests/probes/fat-normalization-f0/fatparam.c -o /tmp/fatparam && /tmp/fatparam

Status 2026-07-30: all 5 properties PASS, sanitizer-clean. Not part of
`tests/run.sh` (no `expected.*`); a one-shot stage-0 artifact kept for
reproducibility, per the probe-vs-fixture split in the meta-plan.
