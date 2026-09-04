# Fix: the fuzz harnesses inherited another install's `TUR_STDLIB_DIR`

Resolves [type-fuzz-src-red-on-clang-21](../type-fuzz-src-red-on-clang-21.md).
Investigated and fixed 2026-08-28 on the same box the report was filed from
(Apple clang 21.0.0, arm64-apple-darwin27).

## The answer

`tests/type-fuzz-src.py` compiled every generated case with the **tree's
compiler** against **turmeric 0.36.0's stdlib**. Nothing about clang 21, the
platform, or the SR2 sum lowering was involved.

v0.36.0 spells the type as:

```turmeric
(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))
```

a flat struct. The current compiler's box layout is a tagged union, so the
emitted C reaches for members that do not exist:

```
error: no member named 'is_ok' in 'tur_result_box_t'
    return (tur_adt_Result__int__int){.is_ok = __t172->is_ok, ...};
```

That is the whole of `BUG_invalid_c`. `BUG_wrong_output` is the same mismatch
where the layouts happen to be compatible enough to compile and not to mean the
same thing. It is why **all 15 failures were `res`/`res_box`/`res_bind` and no
Option, Vec, struct, or closure leg failed**: Result/Option are the stdlib types
whose representation moved between 0.36.0 and now.

## Why it was hard to see

`TUR_STDLIB_DIR` was not set in the invoking shell. It was injected by **mise's
`python3` shim** -- `~/.local/share/mise/shims/python3` is a symlink to `mise`,
which re-exports the active tool environment *inside* the process it launches.
So the variable existed only within the harness, and:

- **A saved case did not reproduce by hand.** The report identified this as the
  thing to chase first, and that was the right call -- it is the entire finding.
  Running `tur run <save-dir>/input.tur` from a shell uses the in-tree stdlib
  and prints the expected output.
- **`env -u TUR_STDLIB_DIR python3 ...` did not fix it either**, because the
  shim re-injects the variable after `env` has done its work. Only
  `/usr/bin/python3` (bypassing the shim) or stripping the variable at
  subprocess-spawn time inside the driver actually works.
- **It was stable across compiler versions.** The v0.39.0 comparison and the
  `TUR_SR2_APP_SUM_BYVALUE=0` run both produced byte-identical failing case
  names, which reads as "predates this line of work" and is better read as "the
  compiler was never the variable."
- **CI was green** because CI has no mise -- not because its clang is older.

## How it was found

Bisected by elimination on a single case (`BUG_invalid_c-000002`), each step
ruling out one hypothesis:

1. Sibling `.tur` files in the shared workdir -- ruled out; the case fails alone
   in an empty workdir.
2. Non-deterministic generation -- ruled out; the generated source is
   byte-identical to the saved `input.tur`, and generating case 2 cold matches
   generating it after cases 0 and 1.
3. Filename, cwd, and the preceding `tur check` -- ruled out individually.
4. `/tmp/tur-build` cache state -- ruled out; wiping it changes nothing.
5. Diffing the **emitted C** from the two paths, which showed the harness
   emitting `Option`/`Result` as flat structs and the by-hand run emitting
   tagged unions. Two different stdlibs, therefore an environment difference.
6. Diffing `os.environ` against the shell's `env`: one turmeric-related key
   present only in the Python view.

Step 5 is the one that should have come earlier. Once the two runs disagree on
the *generated code* rather than on the *result*, the question stops being "what
is wrong with the compiler" and becomes "what are these two compilations
reading", which is a much smaller search.

## The fix

`env.pop("TUR_STDLIB_DIR", None)` at the single subprocess choke point in
`tests/type-fuzz-src.py` (`run_case`) and `tests/refine-fuzz-src.py`. Dropping
the variable rather than pinning it to `<repo>/stdlib` is deliberate: the
compiler's own walk-up then finds the stdlib beside whatever binary is under
test, which keeps `--tur <other-worktree>/build/tur` comparisons honest.

It must be done at spawn time. A shell-level `unset` in `run-*.sh` does not
reach a shimmed interpreter.

Verified through the shimmed `python3`, which is the failing configuration:
`bash tests/run-type-fuzz-src.sh` -> self-test PASS, 40/40 ok, 0 BUG classes.
`bash tests/run-refine-fuzz-src.sh` -> PASS, 0 soundness bugs.

`tests/run.sh` was never affected: its three `python3` calls are static
source-analysis ratchets that do not spawn `tur`.

## What this exposed, and did not fix

`resolve_stdlib_root` (`src/main.c:263`) validates `TUR_STDLIB_DIR` by probing
for a readable `macros.tur` and nothing else, so a stale-but-intact install
passes. The guard's own comment anticipates that the variable "outlives the
install that set it" -- it defends against a *deleted* stdlib, not a
*mismatched* one, and a version-mismatched stdlib is the more likely case and
the one that fails much further downstream. Filed as
`stdlib-dir-guard-accepts-mismatched-stdlib`; not fixed here because the stdlib
carries no version stamp to compare against, so closing it means adding one.
