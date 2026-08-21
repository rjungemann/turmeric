# tur audit is promised by the security docs but not implemented

**Severity: low**. Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `tur audit` exists.

## Repro

consuming-spices-guide's security section: "`tur audit` (planned) will list
all cmake-dep repositories and their maintainer GPG keys". No `audit`
subcommand existed -- only `audit-spans`, an unrelated debugger mode.

## Resolution

`cmd_pkg_audit` in src/compiler/pkg.c, wired into `CANONICAL_COMMANDS` and
the dispatch chain. It reads `build.tur` plus `tur.lock` and prints, for every
origin the build fetches code from:

- Turmeric spices and `:cmake-deps` in separate sections, with the cmake
  section headed by what it means -- each one executes build scripts from its
  repository, which is the guide's own framing of why this matters.
- the URL, ref, and `:subdir`;
- the resolved commit and SHA-256 when `tur.lock` has pinned it;
- `via system package <version>` when the lock says it resolved that way, and
  a note on `:prefer-system` deps that a system copy bypasses the pin;
- `NOT IN tur.lock -- run \`tur fetch\` to pin it` otherwise, plus a summary
  line when anything is unpinned.

A `:path` dep is listed but **not** flagged as unpinned. It resolves from
local source and has nothing to pin, so flagging it would train the reader to
ignore the warning that does matter.

## What it deliberately does not do

**It verifies nothing, and says so** on every run. The guide's original wording
promised maintainer GPG keys; there is no key infrastructure in the tree to
check against, and printing an unverified key would be worse than printing
none -- it would read as a verification that never happened. The command
reports exactly what the manifest and lock already say, which is a real answer
to "what am I trusting, and is it pinned?" without overclaiming.

## Prefix resolution

`audit` is now a prefix of nothing else, but `au` is ambiguous between `audit`
and `audit-spans`, so `tur au` prints usage rather than guessing -- which is
what `resolve_command`'s existing `COMMAND_AMBIGUOUS` path already does.
`tur audit-spans` is an exact match and is unaffected; checked against a real
file after the change.

Shell completion needed no edit: it derives from `CANONICAL_COMMANDS`, so
`tur completion zsh` lists `audit` automatically.

## Tests

`tests/spice-resolver-tests.sh`, cases `AUDIT1` and `AUDIT2`. The first runs
with no `tur.lock` and asserts both `:url` origins are flagged, the `:path`
dep is listed but not flagged, and the "verifies nothing" disclaimer is
present. The second adds a lock file in `pkg_lock_write`'s own format and
asserts each origin now reports its commit and hash and nothing is flagged.

Suites: run.sh 2676 passed / 0 failed; spice-resolver-tests.sh 80 passed / 0
failed; run-cli.sh and run-flags.sh unchanged.

## Guides updated

- docs/guides/consuming-spices-guide.md -- the "(planned)" bullet is replaced
  by what the command does, an invocation, and the explicit
  lists-but-does-not-verify caveat, including why the GPG-key half is not
  there.
- docs/guides/package-management-guide.md -- `tur audit` added to the Common
  CLI Reference.
- `tur --help` gained a line for it.
