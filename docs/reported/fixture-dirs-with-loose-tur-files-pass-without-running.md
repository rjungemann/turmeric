# Four fixture directories report PASS without running anything (30 `.tur` files)

**Severity:** medium -- silent coverage loss. The suite counts these as passing,
so the loss is invisible in the summary line and in CI.

## Summary

`tests/run.sh` discovers a fixture directory, then looks for its input as
`<dir>/input.tur` or `<dir>/<dirname>.tur`. A directory that holds **loose
`.tur` files under other names** matches neither, hits the fallback, and is
reported as **PASS**:

```sh
else echo "SKIP $name (no input)" ; write_result "PASS" "$name" "(no input -- skipped)" "" ; return; fi
```

The `SKIP` line goes to the live progress stream; the recorded result is `PASS`.

## Repro

```
$ TUR_TEST_FILTER='^(stm|typeclass|sandbox|module-transitive-imports)$' bash tests/run.sh
SKIP sandbox (no input)
SKIP module-transitive-imports (no input)
PASS sandbox
SKIP typeclass (no input)
PASS module-transitive-imports
PASS typeclass
SKIP stm (no input)
PASS stm

summary: 4 passed, 0 failed
```

## What is not running

| Directory | `.tur` files | Contents |
|---|---|---|
| `sandbox/` | 17 | `sb-unsafe-cast`, `sb-transmute`, `sb-dlopen`, `sb-ptr-arith`, `sb-raw-malloc`, `sb-step-limit`, ... |
| `stm/` | 7 | `stm-atomicity`, `stm-deadlock-free`, `stm-or-else`, `stm-read-outside-transaction`, `stm-write-outside-transaction`, `stm-tvar-basic`, `stm-tvar-simple` |
| `module-transitive-imports/` | 4 | a `src/` tree exercising transitive module imports |
| `typeclass/` | 2 | `parametric-clone-list`, `parametric-clone-pair` |

30 files total. No other harness picks them up -- `grep` for these paths across
`tests/*.sh` and `CMakeLists.txt` returns nothing, so they are covered by
nothing at all. The STM set is the most concerning: atomicity and deadlock
freedom are exactly the properties a silent skip should not be hiding.

## Root cause

`tests/run.sh`, the discovery loop. A directory is classified as a *group*
(whose children are fixtures) only when it holds **no regular file of its own**:

```sh
for _f in "$d"/*; do
    [ -f "$_f" ] && { _has_own_file=1; break; }
done
if [ "$_has_own_file" = 0 ]; then
    for sub in "$d"/*/; do
        [ -f "${sub}input.tur" ] && { _is_group=1; break; }
    done
fi
```

- `stm/`, `typeclass/`, `sandbox/` hold loose `.tur` files, so `_has_own_file=1`
  and they are treated as single fixtures -- then have no `input.tur`.
- `module-transitive-imports/` holds only a `src/` directory, so
  `_has_own_file=0`, but `src/` contains no `input.tur`, so it is not a group
  either -- and falls into the same hole from the other side.

The loop's own comment says the set inclusion is "asserted below rather than
assumed" precisely because of "invisible-coverage-loss", so the intent is right;
the fallback just answers PASS where it should answer "this directory holds
fixtures I do not know how to run".

## Fix directions

Two independent halves, and the first is worth doing even alone:

1. **Stop reporting PASS for a directory with no runnable input.** Either FAIL
   it, or add it to an explicit allow-list of known-non-fixture directories.
   Anything but a silent PASS. This is a few lines and it converts every future
   instance of this from invisible to loud.
2. **Decide what these four directories are.** Either give each `.tur` its own
   fixture directory (`stm/stm-atomicity/input.tur` + `expected.stdout`, which
   makes the group-dir branch pick them up), or teach the runner a
   loose-`.tur`-files layout. The first needs no runner change and matches how
   `spices/` is already laid out.

Note that `stm/`, `sandbox/`, and `typeclass/` have no `expected.stdout` of any
kind, so reviving them means deciding what each asserts -- they may have been
written against an older harness. Check whether they still pass before treating
a failure as a regression.

## Found while

Writing the two-module fixtures for G3 of
[docs/upcoming/mutable-globals-plan.md](../upcoming/mutable-globals-plan.md),
which needed to know how a multi-module fixture is laid out.
`module-transitive-imports` looked like the precedent and turned out not to run.
