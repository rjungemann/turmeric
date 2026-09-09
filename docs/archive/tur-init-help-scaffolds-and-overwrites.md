---
title: "`tur init --help` scaffolds instead of printing help, overwriting an existing `.gitignore` and `README.md`"
category: Reported
description: "RESOLVED. Both defects fixed: `tur init` handles --help and refuses unknown flags, and scaffolding refuses PRE-FLIGHT if any target exists, naming them, with --force as the opt-in. The refusal covers five targets, not the two reported -- the manifest, tur.lock, the Justfile and ci.yml each write through their own writer and bypassed the first version of the fix."
---

# `tur init --help` scaffolds a project over your working tree

**RESOLVED 2026-09-08.** Both defects fixed.
`tests/run-init-no-clobber.sh` / ctest `tur_init_no_clobber` pins it, and was
verified to reproduce the data loss against the unfixed binary.

**Severity was high.** Not a crash and not a wrong answer -- it was DATA LOSS in
a developer's working tree, from a command spelled `--help`. Two independent
defects composed into it, and either alone would have been much less bad.

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

## Fix directions -- 1 and 2 taken, 3 rejected

1. **Handle `--help` in `cmd_pkg_init`. DONE**, matching every other
   subcommand. Widened by one case the report did not name: an UNKNOWN flag was
   also silently ignored, so `tur init --saffron` -- looking for a flag that
   does not exist, which is exactly how this was found -- scaffolded just as
   `--help` did. Unknown flags are now refused.

2. **Never overwrite an existing file. DONE**, as a PRE-FLIGHT refusal that
   names every colliding target, with `--force` as the documented opt-in.

   Pre-flight rather than per-file so a collision cannot leave a
   half-scaffolded tree, and it reuses the real scaffold walk in a probe pass
   (silent, no mkdir, no writes) rather than re-listing the target paths --
   which would drift the first time a scaffold file is added.

3. ~~**Refuse in a non-empty directory unless `--force`.**~~ Rejected on
   inspection: `.git` makes any repository non-empty, so `git init && tur init`
   -- a perfectly ordinary sequence -- would refuse. Direction 2 with a probe
   pass gets the same drift-resistance without that false positive.

## What the first version of the fix missed

Worth recording, because it is the reason the fix is bigger than the report
implied. `scaffold_write` looked like a single chokepoint every target goes
through. It is not: the manifest, `tur.lock`, the `Justfile` and
`.github/workflows/ci.yml` each write through their own writer
(`pkg_manifest_write`, `pkg_lock_write`, `justrun_write_template`, and a direct
`scaffold_write` call outside the collision path). So the first version left all
four still overwritable, and printed their names during what was supposed to be
a silent probe.

The stray output is what surfaced it -- four filenames appearing after a
refusal that had already returned. Had the probe been silent, the gap would have
shipped looking correct.

Five targets are covered now, and the harness asserts each by name for exactly
this reason.

## Not this bug

`tur init --sweet` and `tur new` were and are fine --
`tests/run-sweet-manifest.sh` and `tests/run-tur-new.sh` both scaffold inside
`mktemp -d`, which is why no test caught this. Nothing in the suite ran
`tur init` in a directory it cared about, and nothing checked that `--help`
prints help. `tests/run-init-no-clobber.sh` now does both.
