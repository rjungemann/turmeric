# tur audit is promised by the security docs but not implemented

**Severity: low**. Found in the 2026-08-20 docs audit.

## Repro

consuming-spices-guide's security section: "`tur audit` (planned) will list
all cmake-dep repositories and their maintainer GPG keys". No `audit`
subcommand exists (only `audit-spans`).

## Fix direction

Implement `tur audit` listing `:cmake-deps`/`:spices` origins from build.tur
+ tur.lock.

## Guides to update when fixed

- docs/guides/consuming-spices-guide.md (Security section)
