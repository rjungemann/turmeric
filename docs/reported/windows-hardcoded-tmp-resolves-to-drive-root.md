# Hardcoded `/tmp` paths resolve to the drive root in native Windows binaries

**Summary:** `stdlib/fs.tur`'s `fs/tmpfile` does `mkstemp("/tmp/tur_XXXXXX")`,
and around a dozen fixtures write hardcoded `"/tmp/..."` paths. Compiled
turmeric programs are **native** Windows executables, not MSYS-linked, so their
CRT resolves `/tmp/foo` against the **current drive root** -- `C:\tmp\foo`,
`D:\tmp\foo` -- not against the MSYS shell's `/tmp` (which is
`%LOCALAPPDATA%\Temp`). When that directory does not exist the open fails, the
program prints nothing, and the fixture reports a stdout mismatch that gives no
hint the cause is a missing temp directory.

**Severity:** Medium. Not a wrong answer -- the I/O simply fails -- but it hits
`fs/tmpfile`, which is public stdlib API, so it affects real Windows users and
not only fixtures. The diagnosis is also expensive: the symptom is an empty
stdout.

**Platform:** Windows only.

## How it surfaced (worth reading before trusting a green Windows suite)

The suite was **2781 passed, 0 failed** on a developer machine and **2769
passed, 12 failed** on a fresh CI runner, from the same commit.

The developer box happened to have `C:\tmp` -- created by earlier runs of these
very fixtures -- and the repo lives on `C:`. The runner's workspace is on `D:`,
where `D:\tmp` does not exist. So the green run was green because of leftover
state, and every one of the 12 failures was a real portability bug that the
local run could not see.

## Affected

All twelve share this single root cause:

```
async-file                 image-reload-hook          serial-typed-surface
httpd-mw-static            image-roundtrip            tmpfile-linear-borrow
image-globals-roundtrip    inline-c-struct-return-cstr-params
image-hooks-tracked        io-stdlib-roundtrip        warn-image-global-unregistered
```

(`httpd-async-limit` also failed on that run, but for an unrelated reason -- a
load-sensitive 10s timeout.)

## Current mitigation, and why it is not the fix

`tests/run.sh` now creates `<workspace-drive>:\tmp` on Windows before running
anything. That is *provisioning*, in the same spirit as installing `diffutils`
for the harness -- it makes the suite honest on any machine, but it does not
make `fs/tmpfile` correct for a user who never runs the suite.

## Status

**Step 1 is done** (`stdlib/fs.tur`): the temp directory is resolved at run time
-- `GetTempPathA` + `_mktemp_s` + `_open(_O_EXCL)` on Windows, `TMPDIR` else
`/tmp` on POSIX. Verified by printing what `fs/tmpfile` actually returns, since
the fixtures pass either way on a machine that already has `C:\tmp`:

```
before:  C:\tmp\tur_XXXXXX        (returned 0 outright where C:\tmp is absent)
after:   C:\Users\<u>\AppData\Local\Temp\tur_a10656
```

Steps 2 and 3 remain, and they are ordered: the harness provisioning cannot be
removed until the ten fixtures below stop spelling `/tmp` literally.

## Fix directions

1. **DONE.** ~~`fs/tmpfile` should not hardcode `/tmp`.~~ Under `_WIN32` use
   `GetTempPathA` (which honours `TMP`/`TEMP`/`USERPROFILE` and always exists)
   and `_mktemp_s`, or simply `_tempnam(NULL, "tur_")`. MinGW has no `mkstemp`
   contract to preserve here, and this is the change that helps actual users.
2. **Fixtures should stop spelling `/tmp` literally.** Either give them a
   `TUR_TEST_TMPDIR` the harness sets per-fixture, or route them through
   `fs/tmpfile` once (1) is portable. Prefer this over per-fixture `#ifdef`.
3. Once both land, delete the provisioning block in `tests/run.sh` and re-run on
   a machine with no drive-root `\tmp` -- that absence is the actual test.

## Related

- [win64-aggregate-return-threshold-is-sysv.md](win64-aggregate-return-threshold-is-sysv.md)
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)
