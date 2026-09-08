---
title: "`tur init --help` scaffolds instead of printing help, overwriting an existing `.gitignore` and `README.md`"
category: Reported
description: "`tur init --help` does not print usage. It treats the current directory's name as the project name and scaffolds, and scaffolding overwrites a pre-existing .gitignore and README.md without refusing or asking. Run in a checkout of this repo it destroyed 209 lines of .gitignore, which then unmasked thousands of test artifacts as untracked."
---

# `tur init --help` scaffolds a project over your working tree

**Severity: high.** Not a crash and not a wrong answer -- it is DATA LOSS in a
developer's working tree, from a command spelled `--help`. Two independent
defects compose into it, and either alone would be much less bad.

Found by running `tur init --help` in a checkout of this repository, looking for
a `--saffron` flag.

## What happened

```
$ ./build/tur init --help
$ git status --short
 M .gitignore
 M README.md
?? build.tur
?? src/main.tur
?? tests/turmeric_test.tur
?? tur.lock
... ~2000 more untracked files
```

`.gitignore` went from 209 lines to 8 -- every `build-*/`, `*.dSYM/`, `*.o`
pattern gone, replaced by the scaffold template. `README.md` became
`# turmeric` plus scaffold boilerplate. The ~2000 extra untracked entries are
not new files: they are test-run artifacts (`actual.stdout`, `turi.stderr`,
`Testing/`, `__pycache__/`) that the real `.gitignore` had been covering, now
unmasked.

Both overwritten files are TRACKED, so `git checkout` recovered them. In a
checkout with uncommitted edits to either, the edits are simply gone.

## The two defects

**1. `--help` is not handled.** `cmd_init` never checks for it. The flag falls
through to the project-name slot, and with no name given the current
directory's basename is used instead. So `tur init --help` in a directory
called `turmeric` scaffolds a project called `turmeric`.

Measured in a temp dir, where the directory name happens to be invalid:

```
$ cd /tmp/tmp.Df2Vz5M8ZM && tur init --help
tur init: invalid project name 'tmp.Df2Vz5M8ZM'
  Names must match [a-z][a-z0-9-]* ...
```

That diagnostic is the only reason this is not worse: a temp dir usually has a
name the validator rejects, so the accident is invisible until someone runs it
somewhere with a plausible directory name -- like a project checkout.

**2. Scaffolding overwrites existing files without refusing.** Reproduced in a
fresh git repo with two committed files:

```
$ mkdir myproj && cd myproj
$ printf 'REAL GITIGNORE\nbuild/\n' > .gitignore
$ printf '# My Real README\n' > README.md
$ git init -q . && git add -A
$ tur init --help          # exit 0
$ head -1 .gitignore       # build/          <- "REAL GITIGNORE" gone
$ head -1 README.md        # # myproj        <- "# My Real README" gone
```

There IS a refusal path -- `tests/run-sweet-manifest.sh` SW5 checks that
`tur init` refuses when `build.tur.sweet` already exists -- but it is keyed on a
MANIFEST being present, not on the directory being non-empty or the files being
about to be clobbered. A project checkout with no `build.tur` at its root sails
straight past it.

## Fix directions

1. **Handle `--help` in `cmd_init`.** One arm, matching every other subcommand.
   Closes the accident that makes defect 2 reachable by someone who typed a
   flag, not a command.
2. **Never overwrite an existing file.** Scaffold only files that do not exist,
   and refuse (naming them) if any target is present -- rather than keying the
   refusal on a manifest. `.gitignore` and `README.md` are the two that matter,
   because they are the two a real project is most likely to already have and
   least likely to have in a `build.tur`-less state by accident.
3. **Refuse in a non-empty directory unless `--force`.** The blunter version of
   2. Worth considering because it also covers scaffold files added later
   without anyone remembering to extend the do-not-clobber list.

1 and 2 are both small and independent; 2 is the one that makes this
non-destructive even when some future flag parsing bug reaches it again.

## Not this bug

`tur init --sweet` and `tur new` are fine -- `tests/run-sweet-manifest.sh` and
`tests/run-tur-new.sh` both scaffold inside `mktemp -d`, which is why no test
caught this. Nothing in the suite runs `tur init` in a directory it cares
about, and nothing checks that `--help` prints help.
