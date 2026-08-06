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

---

## Execution -- RESOLVED 2026-08-05

Both fix directions were taken. The repro reproduced exactly as written.

### Correction: the count is 13 files, not 30, and one claim is wrong

The report says of all four directories: *"No other harness picks them up --
grep for these paths across `tests/*.sh` and `CMakeLists.txt` returns nothing,
so they are covered by nothing at all."*

**`sandbox/` is covered.** All 17 of its `sb-*.tur` files are run by the
`tur_eval_sandbox` ctest target, whose fixture list and expected outcomes live
in `tests/turi/sandbox-eval.c` -- a **C source**, which is why a grep restricted
to `tests/*.sh` and `CMakeLists.txt` missed it. `./build/tur_eval_sandbox`
reports `21 passed, 0 failed` today. The report singles the sandbox set out
alongside STM as "the most concerning"; it was never at risk.

That leaves `stm/` (7), `typeclass/` (2) and `module-transitive-imports/` (4) --
**13 files** genuinely covered by nothing. The STM half of the concern stands.

### The bigger finding: 23 directories, and the marker for this already existed

Enumerating every dir that reaches the fallback turned up **23**, not 4. Of
those, 20 are legitimately driven by another harness -- and **17 of them already
carried a `requires.dedicated-runner` marker saying so.**

That marker was unreachable. `run_happy` looked for the input *first* and
returned at the no-input branch, so the marker check three lines below never ran
for exactly the directories that carry it. The silent PASS was answering on
their behalf, which is why nobody noticed the order was wrong.

So the fix is smaller than the report assumes: **check the marker before the
input lookup**, then fail anything still without one. `run_negative` gets the
same treatment (it had no marker check at all).

### What changed

**`tests/run.sh`** -- the no-input branch is now `no_input_fail`, a shared helper
that FAILs with a log explaining the four ways a fixture dir may declare how it
runs (`input.tur` / `<dirname>.tur`, `hook.sh`, `requires.dedicated-runner`, or
per-file subdirectories) and notes the one non-fixture case: a directory holding
only generated artifacts left by a deleted fixture.

**Markers for the three dirs that had none but are owned:** `sandbox/`
(`tur_eval_sandbox`) and `spices/` (a tree of spice packages consumed as `:path`
deps, not a fixture). Each marker file names its owner, since the runner only
tests for the file's existence.

**`stm/` -- 5 fixture dirs plus 2 `errors/` negatives.** Every one of these
called a function at top level and **discarded the result**, so even when run
they asserted nothing beyond "does not crash". The values are the assertions
they were missing. A TVar's value is an int64 boxed as `ptr<void>` that
`println` will not take, so they read back through `(:: v :int)`.
`stm-atomicity` is the notable one: it claimed to "verify writes are not visible
until commit" while reading the TVar *inside* the writing transaction, so it
could not have distinguished read-your-writes from a committed read. It now
tests both, plus a `cas` against a stale value.

**`typeclass/` -- 2 fixture dirs, and both were broken.** `parametric-clone-pair`
named its struct `Pair`, which the auto-loaded stdlib already defines, and the
redefinition cascaded into a spurious TUR-E0013 orphan-instance error;
renamed. `parametric-clone-list` failed in the **C compiler**: its body was
`(clone [x] x)` against a method declared `: int`. Both now clone through the
`Clone int` constraint and assert a value.

**`module-transitive-imports/`** gets a `hook.sh` (`tur run src/main.tur -I src`,
expecting `3`), which is how a project fixture with no top-level `input.tur`
declares itself.

**`hello/`** turned up in the sweep and was not in the report: a tracked
`expected.stdout` containing `hi`, with no `input.tur` and no harness
referencing it -- a fixture whose input had gone missing. Revived as the
one-line program that produces it.

Its input had not been deleted -- it could never have been committed.
`.gitignore` carried a block of bare names (`tur`, `hello`, `input`, `primes`,
`sort`, ...) for stray binaries left by ad-hoc builds in the repo root, and an
unanchored gitignore pattern matches a path component of that name at **any**
depth. `hello` therefore ignored the whole `tests/fixtures/hello/` directory;
`expected.stdout` survives only because it was already tracked when the rule
landed. All eight are now anchored with a leading `/`, which is what they meant
in the first place. Worth knowing as a shape: a fixture whose files silently
refuse to be added is not a git accident, it is a too-broad ignore rule, and the
silent PASS is what let the half-fixture sit there afterwards.

**One stale artifact directory removed:** `errors/effect-handler-clause-loop-unsupported/`
held only gitignored `actual.stderr`/`turi.stderr` left by a fixture deleted in
`db094dff`. Nothing tracked; it would fail the new check on any tree that had
run the suite before that commit.

### Two defects this uncovered, filed separately

Running the 13 files for the first time found two things, neither of which is
this report's subject:

- [struct-return-type-mismatch-unchecked-until-cc](../reported/struct-return-type-mismatch-unchecked-until-cc.md)
  -- `(defn f [x : S] : int x)` passes `tur check` and dies in `cc`. Primitive
  mismatches of the same shape are caught; the struct/ADT arm of the return-type
  comparison is missing. This is what `parametric-clone-list.tur` was sitting
  on, and it reproduces with no typeclass involved.
- [ascribe-bool-to-int-prints-differently-per-path](../reported/ascribe-bool-to-int-prints-differently-per-path.md)
  -- `(:: b :int)` prints `1`/`0` compiled and `true`/`false` interpreted, so a
  fixture using it cannot have one `expected.stdout` both harnesses accept.

A stale comment was also corrected: `tests/turi/eval-stm.tur` declared itself
interpreter-only because `tvar/cas`/`tvar/swap` failed to link, and pointed at a
`docs/reported/` path that no longer exists. That was fixed in `7b8fd8e9`; the
file runs compiled today, and `tests/fixtures/stm-cas/` is the compiled-path
guard.

### Result

`bash tests/run.sh` 2584 passed, 0 failed (from 2578: 9 new fixture dirs
replacing 4 that ran nothing). `bash tests/run-turi.sh` 1775 passed, 0 failed,
705 skipped. Every fixture directory now either runs or names the harness that
owns it, and a future one that does neither fails loudly instead of counting
itself as a pass.
