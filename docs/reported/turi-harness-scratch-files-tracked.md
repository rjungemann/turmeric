# 39 turi-harness scratch files are git-tracked despite being .gitignored

**Summary:** `tests/run-turi.sh` writes per-run scratch output to
`<fixture>/turi.stdout` and `<fixture>/turi.stderr`. These paths are listed in
`.gitignore` (lines 95-99), but 39 such files are already tracked in the repo --
they were committed before the ignore rule was added, and `.gitignore` does not
untrack already-tracked files. Every harness run that exercises one of those
fixtures rewrites the tracked file, producing spurious `git status` churn.

**Severity:** Low / ergonomics. Not a miscompile and not a CI failure, but it
silently dirties the working tree on a normal `bash tests/run-turi.sh`, which
can mask real changes, get accidentally committed, and confuse contributors.
Surfaced while landing the W5 allowlist->denylist flip: the flip's first
(temporarily broken) run rewrote `tests/fixtures/typed/zipper-basic/turi.{stdout,stderr}`,
which then showed up as unrelated modifications.

## Repro

```sh
git ls-files 'tests/fixtures/**/turi.stdout' 'tests/fixtures/**/turi.stderr' | wc -l   # => 39
bash tests/run-turi.sh >/dev/null 2>&1
git status --short                                                                     # => one or more turi.std* show as " M"
```

Observed: 39 tracked scratch files; running the harness marks some modified.
Expected: zero tracked scratch files; the harness output stays untracked
(matching the existing `.gitignore` intent).

## Root cause

`.gitignore` only prevents *new* files from being staged; files committed
before the pattern existed remain tracked. The 39 `turi.stdout`/`turi.stderr`
files predate the ignore rule. The harness (`run_turi_fixture` in
`tests/run-turi.sh`) writes `$dir/turi.stdout` / `$dir/turi.stderr` for every
fixture it runs, so any tracked one is rewritten in place.

## Proposed fix

```sh
git rm --cached $(git ls-files 'tests/fixtures/**/turi.stdout' 'tests/fixtures/**/turi.stderr')
git commit -m "Stop tracking turi-harness scratch output (already gitignored)"
```

This untracks the 39 files while leaving them on disk; the existing `.gitignore`
rules then keep them out of future commits.

## Validate

After the `git rm --cached` + commit, `git ls-files` for those globs returns
empty and a fresh `bash tests/run-turi.sh` leaves `git status` clean.
