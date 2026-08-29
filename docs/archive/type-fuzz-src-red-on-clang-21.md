# `tur_type_fuzz_src` is red on Apple clang 21, green in CI

**RESOLVED 2026-08-28. NOT A COMPILER BUG, and not about clang 21 at all.**

The harness inherited `TUR_STDLIB_DIR=~/.local/share/mise/installs/turmeric/0.36.0/stdlib`
and compiled the tree's compiler against a **v0.36.0 stdlib**. That release
spells the type as `(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))`
-- a flat struct -- which is exactly where `no member named 'is_ok' in
'tur_result_box_t'` comes from, and why all 15 failures were Result shapes.

The variable is injected by **mise's `python3` shim**, so it exists only inside
the harness process: the invoking shell does not have it, which is precisely
why "a saved case does not reproduce by hand" (the trap this report flagged as
the thing to chase first -- correctly). It also explains why the finding was
identical on the v0.39.0 compiler and under `TUR_SR2_APP_SUM_BYVALUE=0`: the
compiler half was never the variable.

Fixed by stripping `TUR_STDLIB_DIR` from the subprocess environment in
`tests/type-fuzz-src.py` and `tests/refine-fuzz-src.py`. It has to be done at
spawn time, not with `unset` in the shell wrapper -- the shim re-injects it
inside the process. `bash tests/run-type-fuzz-src.sh` now reports 40/40 ok.

Fix direction 3 ("pin the CI toolchain forward, or add a clang-21 leg") is
**withdrawn**: CI was green because CI has no mise, not because of its clang.

Paper trail:
[history/type-fuzz-src-stale-tur-stdlib-dir.md](history/type-fuzz-src-stale-tur-stdlib-dir.md).
The compiler-side gap this exposed -- `resolve_stdlib_root` accepts a
stale-but-intact stdlib because it only probes for a readable `macros.tur` --
is filed separately as `stdlib-dir-guard-accepts-mismatched-stdlib`.

**Severity: medium (a suite that CI cannot see is failing; every finding is a
Result-shape miscompile).** Filed 2026-08-28 while cutting v0.40.0.

## Summary

`bash tests/run-type-fuzz-src.sh` (ctest `tur_type_fuzz_src`) reports **15 of
40 cases as BUG** on a local macOS box running Apple clang 21.0.0
(`arm64-apple-darwin27.0.0`), and **passes in CI on both** `ubuntu-latest`
(43.9s) and `macos-latest` (36.3s). The divergence is the toolchain, not the
platform: CI's macOS image ships an older clang.

```
type_fuzz_src: 40 cases
  BUG_invalid_c                : 6
  BUG_wrong_output             : 9
  ok                           : 25
```

**Every one of the 15 is a Result shape** -- the leg tags are exclusively
`res`, `res_box`, `res_bind`. No Option, Vec, struct, or closure leg fails.

## It is not the v0.40.0 work

Established three ways, all with the same seed (deterministic, `--jobs 1`):

- `TUR_SR2_APP_SUM_BYVALUE=0` (the SR2a graduation's escape hatch, which
  restores the int64 carrier for parametric sum monomorphs): **identical 15**.
- `TUR_CC_FLAGS="-O2 -std=c99 -fno-strict-aliasing"` (the flags
  `tests/run.sh` sets and this harness does not): **identical 15**.
- The **v0.39.0 compiler and its stdlib**
  (`--tur <main-worktree>/build/tur`, which predates the graduation, the
  `ok?`/`err?` retyping, and SR3 slice B): **identical 15, and the failing
  case names are byte-identical** --

```
BUG_invalid_c-000002 000011 000018 000021 000028 000038
BUG_wrong_output-000000 000003 000004 000005 000012 000014 000022 000026 000037
```

So the finding predates this line of work and is stable across it.

## The trap that cost the most time

**A saved case does not reproduce by hand.** Running the harness's own
`input.tur` with the harness's own command (`tur run <path>`, cwd = repo root,
same env) prints the *expected* output:

```
$ ./build/tur run <save-dir>/BUG_wrong_output-000000/input.tur
true
20
true          # the README records got: 'true\n-9999\ntrue\n'
```

The same is true of the bisected `leg1.tur` alone. Whatever the harness is
doing differently, it is not the file contents, the filename, the working
directory, `--jobs`, or `TUR_CC_FLAGS`. Anyone reducing this should start by
finding that difference, not by reading the generated program: the program is
fine when you compile it yourself.

## Repro

```sh
python3 tests/type-fuzz-src.py --n 40 --seed 1 --jobs 1
```

Compare against CI's run of the same command on the same commit.

## Fix directions

1. Find why a saved case passes by hand and fails in-harness -- that gap is
   the actual bug, and it may be in the harness rather than the compiler.
2. If it is the compiler: all 15 are Result shapes, so start at the SR2b sum
   lowering and the `res_bind` legs (6 of the 15, and the only
   `BUG_invalid_c` source), with a clang-21 build.
3. Pin the CI toolchain forward, or add a clang-21 leg. CI's green is
   currently a statement about an older clang, and this will land in CI on its
   own schedule when the runner image moves.
